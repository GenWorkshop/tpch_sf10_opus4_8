#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <algorithm>
#include <immintrin.h>

// AVX2 left-pack: emit (tile-relative) indices of lanes whose date is in [lo,hi).
__attribute__((target("avx2")))
static inline int q15_filter_tile_avx2(const int32_t* __restrict sd, int n,
                                       int32_t lo, int32_t hi,
                                       int32_t base, int32_t* __restrict out) {
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
    const __m256i vlo1 = _mm256_set1_epi32(lo - 1); // d > lo-1  <=>  d >= lo
    const __m256i vhi  = _mm256_set1_epi32(hi);
    const __m256i iota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    int count = 0;
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i d  = _mm256_loadu_si256((const __m256i*)(sd + i));
        __m256i m1 = _mm256_cmpgt_epi32(d, vlo1);
        __m256i m2 = _mm256_cmpgt_epi32(vhi, d);
        __m256i mask = _mm256_and_si256(m1, m2);
        int mm = _mm256_movemask_ps(_mm256_castsi256_ps(mask));
        if (mm) {
            __m256i idx  = _mm256_add_epi32(iota, _mm256_set1_epi32(base + i));
            __m256i perm = _mm256_load_si256((const __m256i*)perm_table[mm]);
            __m256i pkd  = _mm256_permutevar8x32_epi32(idx, perm);
            _mm256_storeu_si256((__m256i*)(out + count), pkd);
            count += __builtin_popcount((unsigned)mm);
        }
    }
    for (; i < n; i++) {
        out[count] = base + i;
        count += (sd[i] >= lo) & (sd[i] < hi);
    }
    return count;
}

inline void run_q15_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q15_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // CTE revenue: l_shipdate >= '1996-02-01' AND l_shipdate < '1996-05-01'
    const Date date_lo = date_to_epoch(1996, 2, 1);
    const Date date_hi = date_to_epoch(1996, 5, 1);

    // Compute total_revenue per supplier using a dense array keyed by suppkey
    // (suppkey is dense 1..n_supplier, so this is a perfect-hash group-by).
    const int32_t n_supp = db->n_supplier;
    std::vector<int64_t> supp_revenue(n_supp + 1, 0); // index = suppkey

    {
        PROFILE_SCOPE("q15_lineitem_scan_agg");
        const Date* __restrict shipdate = db->l_shipdate.data();
        const int64_t* __restrict eprice = db->l_extendedprice.data();
        const int64_t* __restrict disc = db->l_discount.data();
        const int32_t* __restrict suppkey = db->l_suppkey.data();
        int64_t* __restrict acc = supp_revenue.data();
        const int64_t n = db->n_lineitem;

        constexpr int TILE = 8192;
        alignas(64) int32_t surv[TILE + 8];
        for (int64_t b = 0; b < n; b += TILE) {
            int tn = (int)std::min<int64_t>(TILE, n - b);
            int c = q15_filter_tile_avx2(shipdate + b, tn, date_lo, date_hi,
                                         (int32_t)b, surv);
            TRACE_ADD(li_scanned, tn);
            TRACE_ADD(li_emitted, c);
            for (int k = 0; k < c; k++) {
                int32_t idx = surv[k];
                acc[suppkey[idx]] += eprice[idx] * (100 - disc[idx]);
            }
        }
    }
    TRACE_COUNT("q15_rows_scanned", li_scanned);
    TRACE_COUNT("q15_rows_emitted", li_emitted);
    TRACE_COUNT("q15_agg_rows_in", li_emitted);
    TRACE_COUNT("q15_groups_created", (uint64_t)n_supp);

    // Find max revenue
    int64_t max_rev = 0;
    for (int32_t sk = 1; sk <= n_supp; sk++) {
        if (supp_revenue[sk] > max_rev) max_rev = supp_revenue[sk];
    }

    // Collect suppliers with max revenue (already in suppkey order)
    struct ResultRow {
        int32_t s_suppkey;
        int64_t total_revenue;
    };
    std::vector<ResultRow> results;
    for (int32_t sk = 1; sk <= n_supp; sk++) {
        if (supp_revenue[sk] == max_rev) {
            results.push_back({sk, supp_revenue[sk]});
        }
    }

    // Order by s_suppkey
    TRACE_COUNT("q15_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q15_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.s_suppkey < b.s_suppkey;
        });
    }
    TRACE_COUNT("q15_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q15_output");
    write_csv_header(out, {"s_suppkey","s_name","s_address","s_phone","total_revenue"});
    for (auto& r : results) {
        int32_t s_idx = r.s_suppkey - 1;
        write_csv_row(out, {
            std::to_string(r.s_suppkey),
            csv_quote(db->s_name[s_idx]),
            csv_quote(db->s_address[s_idx]),
            csv_quote(db->s_phone[s_idx]),
            fmt_money(r.total_revenue, 4)
        });
    }
    TRACE_COUNT("q15_query_output_rows", (uint64_t)results.size());
}
