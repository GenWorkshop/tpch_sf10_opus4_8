#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q4_impl.hpp" // q4_leftpack_table
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <immintrin.h>

inline void __attribute__((target("avx2"))) run_q5_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q5_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // r_name = 'AFRICA' → regionkey
    int32_t target_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "AFRICA") { target_regionkey = i; break; }
    }

    // Nations in target region → small bool table indexed by nationkey (0..24)
    bool target_nation[64] = {false};
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == target_regionkey) {
            target_nation[i] = true;
        }
    }

    // Date filter: o_orderdate >= '1997-01-01' AND < '1998-01-01'
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    // Per-nation revenue accumulators (scale 4), indexed by nationkey.
    int64_t nation_revenue[64] = {0};

    const int32_t* __restrict o_orderdate = db->o_orderdate.data();
    const int32_t* __restrict o_custkey   = db->o_custkey.data();
    const int32_t* __restrict o_orderkey  = db->o_orderkey.data();
    const int32_t* __restrict c_nationkey = db->c_nationkey.data();
    const int32_t* __restrict s_nationkey = db->s_nationkey.data();
    const int32_t* __restrict l_suppkey   = db->l_suppkey.data();
    const int64_t* __restrict l_extprice  = db->l_extendedprice.data();
    const int64_t* __restrict l_discount  = db->l_discount.data();
    const auto* __restrict ranges = db->orderkey_lineitem_range.data();

    // Join order follows the DuckDB plan bottom-up: the highly selective
    // orders(date) ⋈ customer(nation∈AFRICA) result drives a CSR lookup into
    // lineitem, so we only touch lineitems of qualifying orders instead of
    // scanning all ~60M rows.  The pipeline is phased (q4-style) so each random
    // gather is hidden behind an independent software-prefetch stream with no
    // dependent load on the expensive array.
    {
        PROFILE_SCOPE("q5_lineitem_scan_join_agg");

        // Phase 1: AVX2 date filter, left-packing passing order indices.
        // Range check via (unsigned)(d - lo) < (hi - lo).
        std::vector<int32_t> pass(db->n_orders + 8);
        int32_t* __restrict pbuf = pass.data();
        size_t npass = 0;
        {
            const int32_t* tbl = q4_leftpack_table();
            const uint32_t range = (uint32_t)(date_hi - date_lo);
            const __m256i vlo = _mm256_set1_epi32(date_lo);
            const __m256i vsign = _mm256_set1_epi32((int)0x80000000);
            const __m256i vrange_x = _mm256_set1_epi32((int)(range ^ 0x80000000u));
            const __m256i viota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            int32_t i = 0;
            int32_t n8 = db->n_orders & ~7;
            for (; i < n8; i += 8) {
                __m256i d = _mm256_loadu_si256((const __m256i*)(o_orderdate + i));
                __m256i sub_x = _mm256_xor_si256(_mm256_sub_epi32(d, vlo), vsign);
                __m256i mask = _mm256_cmpgt_epi32(vrange_x, sub_x);
                unsigned m = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(mask));
                __m256i vidx = _mm256_add_epi32(_mm256_set1_epi32(i), viota);
                __m256i perm = _mm256_loadu_si256((const __m256i*)(tbl + m * 8));
                __m256i packed = _mm256_permutevar8x32_epi32(vidx, perm);
                _mm256_storeu_si256((__m256i*)(pbuf + npass), packed);
                npass += (size_t)__builtin_popcount(m);
            }
            for (; i < db->n_orders; i++) {
                pbuf[npass] = i;
                npass += (size_t)((uint32_t)(o_orderdate[i] - date_lo) < range);
            }
        }

        // Phase 2: prefetched gather of customer nationkey, filter to AFRICA.
        // Order-index reads (o_custkey/o_orderkey at ascending pbuf) stay cached;
        // only the c_nationkey gather (6MB, scattered) is prefetched.
        std::vector<int32_t> q_ok(npass);
        std::vector<int32_t> q_nat(npass);
        int32_t* __restrict qok = q_ok.data();
        int32_t* __restrict qnat = q_nat.data();
        size_t nq = 0;
        {
            const int PA = 64;
            for (size_t k = 0; k < npass; k++) {
                if (k + PA < npass)
                    __builtin_prefetch(&c_nationkey[o_custkey[pbuf[k + PA]] - 1], 0, 1);
                int32_t i = pbuf[k];
                int32_t cn = c_nationkey[o_custkey[i] - 1];
                qok[nq] = o_orderkey[i];
                qnat[nq] = cn;
                nq += target_nation[cn];
            }
        }

        // Phase 3: gather lineitem CSR range per qualifying order, prefetched.
        std::vector<Database::LineitemRange> q_rng(nq);
        Database::LineitemRange* __restrict qrng = q_rng.data();
        {
            const int PA = 64;
            for (size_t k = 0; k < nq; k++) {
                if (k + PA < nq) __builtin_prefetch(&ranges[qok[k + PA]], 0, 1);
                qrng[k] = ranges[qok[k]];
            }
        }

        // Phase 4: probe lineitems, accumulate per-nation revenue.  Prefetch the
        // lineitem columns of upcoming orders straight from the compact range
        // array (no dependent load) for maximum memory-level parallelism.
        {
            const int PB = 48;
            for (size_t k = 0; k < nq; k++) {
                if (k + PB < nq) {
                    int32_t s2 = qrng[k + PB].start;
                    __builtin_prefetch(&l_suppkey[s2], 0, 1);
                    __builtin_prefetch(&l_extprice[s2], 0, 1);
                    __builtin_prefetch(&l_discount[s2], 0, 1);
                }
                Database::LineitemRange r = qrng[k];
                int32_t cn = qnat[k];
                int64_t rev = 0;
                for (int32_t li = r.start; li < r.end; li++) {
                    if (s_nationkey[l_suppkey[li] - 1] == cn) {
                        rev += l_extprice[li] * (100 - l_discount[li]); // scale 4
                        TRACE_INC(li_emitted);
                    }
                }
                nation_revenue[cn] += rev;
            }
        }
    }
    TRACE_COUNT("q5_rows_scanned", li_scanned);
    TRACE_COUNT("q5_join_rows_emitted", li_emitted);
    TRACE_COUNT("q5_agg_rows_in", li_emitted);

    // Sort by revenue desc
    struct ResultRow {
        std::string n_name;
        int64_t revenue;
    };
    std::vector<ResultRow> results;
    for (int nk = 0; nk < db->n_nations; nk++) {
        if (nation_revenue[nk] != 0) {
            results.push_back({db->n_name[nk], nation_revenue[nk]});
        }
    }
    TRACE_COUNT("q5_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q5_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q5_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q5_output");
    write_csv_header(out, {"n_name","revenue"});
    for (auto& r : results) {
        write_csv_row(out, {r.n_name, fmt_money(r.revenue, 4)});
    }
    TRACE_COUNT("q5_query_output_rows", (uint64_t)results.size());
}
