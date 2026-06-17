#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <map>
#include <memory>
#include <immintrin.h>

// AVX2 left-pack stream compaction for q12 pass 1: keep the indices of rows
// satisfying l_shipdate < l_commitdate < l_receiptdate AND
// l_receiptdate in [lo, hi).  Computes 8 date predicates per instruction and
// only pays the permute/store when a block contains a survivor.
__attribute__((target("avx2")))
static int64_t q12_pass1_avx2(const int32_t* __restrict rd,
                              const int32_t* __restrict cd,
                              const int32_t* __restrict sd,
                              int64_t n, int32_t lo, int32_t hi,
                              int32_t* __restrict out) {
    alignas(32) static int32_t perm_table[256][8];
    static bool init = false;
    if (!init) {
        for (int m = 0; m < 256; m++) {
            int p = 0;
            for (int b = 0; b < 8; b++) if (m & (1 << b)) perm_table[m][p++] = b;
            for (; p < 8; p++) perm_table[m][p] = 0;
        }
        init = true;
    }
    const __m256i vlo1 = _mm256_set1_epi32(lo - 1); // r > lo-1  <=>  r >= lo
    const __m256i vhi  = _mm256_set1_epi32(hi);
    const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    int64_t count = 0;
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i r = _mm256_loadu_si256((const __m256i*)(rd + i));
        __m256i c = _mm256_loadu_si256((const __m256i*)(cd + i));
        __m256i s = _mm256_loadu_si256((const __m256i*)(sd + i));
        __m256i m1 = _mm256_cmpgt_epi32(r, vlo1); // r >= lo
        __m256i m2 = _mm256_cmpgt_epi32(vhi, r);  // r < hi
        __m256i m3 = _mm256_cmpgt_epi32(r, c);    // c < r
        __m256i m4 = _mm256_cmpgt_epi32(c, s);    // s < c
        __m256i mask = _mm256_and_si256(_mm256_and_si256(m1, m2),
                                        _mm256_and_si256(m3, m4));
        int mm = _mm256_movemask_ps(_mm256_castsi256_ps(mask));
        if (mm) {
            __m256i idx  = _mm256_add_epi32(iota, _mm256_set1_epi32((int)i));
            __m256i perm = _mm256_load_si256((const __m256i*)perm_table[mm]);
            __m256i pkd  = _mm256_permutevar8x32_epi32(idx, perm);
            _mm256_storeu_si256((__m256i*)(out + count), pkd);
            count += __builtin_popcount((unsigned)mm);
        }
    }
    for (; i < n; i++) {
        int32_t r = rd[i], c = cd[i], s = sd[i];
        out[count] = (int32_t)i;
        count += (r >= lo) & (r < hi) & (c < r) & (s < c);
    }
    return count;
}

inline void run_q12_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q12_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // l_receiptdate >= '1997-01-01' AND l_receiptdate < '1998-01-01'
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    // Group by l_shipmode
    struct Agg {
        int64_t high_count = 0;
        int64_t low_count = 0;
    };

    // Two fixed groups (MAIL, FOB) avoid std::map / string-keyed lookups in the
    // hot loop.  Cheap integer date predicates run first so the std::string
    // shipmode column is only touched for the small surviving fraction of rows.
    Agg agg_mail;
    Agg agg_fob;
    const Date* __restrict commitdate  = db->l_commitdate.data();
    const Date* __restrict receiptdate = db->l_receiptdate.data();
    const Date* __restrict shipdate    = db->l_shipdate.data();
    const int32_t* __restrict orderkey = db->l_orderkey.data();
    const std::string* __restrict shipmode = db->l_shipmode.data();

    {
        PROFILE_SCOPE("q12_lineitem_scan_join_agg");
        const int64_t n = db->n_lineitem;
        // Pass 1: branchless collection of rows whose l_receiptdate falls in the
        // target year.  Writing the index unconditionally and bumping the count
        // by the (0/1) predicate eliminates the hard-to-predict range branch that
        // otherwise dominates the 60M-row scan.
        std::unique_ptr<int32_t[]> survivors(new int32_t[n]);
        int64_t count;
        {
        PROFILE_SCOPE("q12_pass1_receiptdate_filter");
        count = q12_pass1_avx2(receiptdate, commitdate, shipdate, n,
                               date_lo, date_hi, survivors.get());
        }

        PROFILE_SCOPE("q12_pass2_probe");

        // Pass 2 split into two lean, monotonic loops so each carries few
        // concurrent random-access streams (better MLP / prefetch).
        // 2a: gather l_shipmode (the only random stream) and compact the
        //     surviving rows' orderkeys + group tag.  ~29% of rows survive.
        // 2b: probe orders for the compacted (dense, ascending) orderkeys.
        const int32_t* __restrict ok2idx = db->orderkey_to_idx.data();
        const std::string* __restrict oprio = db->o_orderpriority.data();
        const int32_t max_ok = db->max_orderkey;

        std::unique_ptr<int32_t[]> buf_ok(new int32_t[count]);
        std::unique_ptr<uint8_t[]> buf_fob(new uint8_t[count]);
        int64_t m = 0;
        {
            constexpr int64_t PFA = 32;
            for (int64_t s = 0; s < count; s++) {
                if (s + PFA < count) {
                    const int32_t j = survivors[s + PFA];
                    __builtin_prefetch(&shipmode[j], 0, 0);
                    __builtin_prefetch(&orderkey[j], 0, 0);
                }
                const int32_t i = survivors[s];
                // Only MAIL starts with 'M' and only FOB with 'F' among the 7
                // TPC-H shipmodes — first char is a unique branchless discriminator.
                const char m0 = shipmode[i][0];
                const bool mail = (m0 == 'M');
                const bool fob  = (m0 == 'F');
                buf_ok[m] = orderkey[i];
                buf_fob[m] = fob ? 1 : 0;
                m += (mail | fob);
            }
        }

        {
            constexpr int64_t PFB = 24;
            for (int64_t k = 0; k < m; k++) {
                if (k + PFB < m) {
                    const int32_t ok = buf_ok[k + PFB];
                    if (ok >= 0 && ok <= max_ok) __builtin_prefetch(&ok2idx[ok], 0, 0);
                }
                const int32_t ok = buf_ok[k];
                if (ok > max_ok) continue;
                const int32_t o_idx = ok2idx[ok];
                if (o_idx < 0) continue;
                TRACE_INC(li_emitted);
                Agg* g = buf_fob[k] ? &agg_fob : &agg_mail;
                // high priority = '1-URGENT' or '2-HIGH' => leading digit <= '2'.
                if (oprio[o_idx][0] <= '2') {
                    g->high_count++;
                } else {
                    g->low_count++;
                }
            }
        }
    }

    std::map<std::string, Agg> groups;
    groups["FOB"] = agg_fob;
    groups["MAIL"] = agg_mail;
    TRACE_COUNT("q12_rows_scanned", li_scanned);
    TRACE_COUNT("q12_join_rows_emitted", li_emitted);
    TRACE_COUNT("q12_agg_rows_in", li_emitted);
    TRACE_COUNT("q12_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q12_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q12_output");
    write_csv_header(out, {"l_shipmode","high_line_count","low_line_count"});
    for (auto& [mode, g] : groups) {
        write_csv_row(out, {
            mode,
            fmt_decimal(static_cast<double>(g.high_count), 1),
            fmt_decimal(static_cast<double>(g.low_count), 1)
        });
    }
    TRACE_COUNT("q12_query_output_rows", (uint64_t)groups.size());
}
