#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <string>
#include <vector>
#include <cstring>
#include <immintrin.h>

__attribute__((target("avx2")))
inline void q14_scan_avx2(int64_t n, const Date* __restrict shipdate,
                          const int32_t* __restrict partkey,
                          const int64_t* __restrict price,
                          const int64_t* __restrict disc,
                          const uint8_t* __restrict promo,
                          Date date_lo, Date date_hi,
                          int64_t& total_sum_out, int64_t& promo_sum_out) {
    int64_t total_sum = 0, promo_sum = 0;
    const __m256i vlo1 = _mm256_set1_epi32(date_lo - 1); // s > lo-1 <=> s >= lo
    const __m256i vhi  = _mm256_set1_epi32(date_hi);     // hi > s   <=> s <  hi

    // Software-pipelined gather: the date window keeps ~1.3% of rows, scattered
    // across the 480MB price/disc arrays.  We prefetch a survivor's columns at
    // discovery and defer the actual gather by RING survivors so the loads have
    // latency to complete.
    constexpr int RING = 32;
    constexpr int MASK = RING - 1;
    int32_t ring[RING];
    int head = 0, cnt = 0;

    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i*)(shipdate + i));
        __m256i m = _mm256_and_si256(_mm256_cmpgt_epi32(s, vlo1),
                                     _mm256_cmpgt_epi32(vhi, s));
        unsigned mm = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(m));
        while (mm) {
            int64_t j = i + __builtin_ctz(mm);
            mm &= mm - 1;
            __builtin_prefetch(&price[j], 0, 0);
            __builtin_prefetch(&disc[j], 0, 0);
            __builtin_prefetch(&partkey[j], 0, 0);
            if (cnt < RING) {
                ring[(head + cnt) & MASK] = (int32_t)j;
                cnt++;
            } else {
                int32_t jo = ring[head];
                int64_t rev = price[jo] * (100 - disc[jo]);
                total_sum += rev;
                promo_sum += rev * (int64_t)promo[partkey[jo] - 1];
                ring[head] = (int32_t)j;
                head = (head + 1) & MASK;
            }
        }
    }
    while (cnt > 0) {
        int32_t jo = ring[head];
        int64_t rev = price[jo] * (100 - disc[jo]);
        total_sum += rev;
        promo_sum += rev * (int64_t)promo[partkey[jo] - 1];
        head = (head + 1) & MASK;
        cnt--;
    }
    for (; i < n; i++) {
        if (shipdate[i] >= date_lo && shipdate[i] < date_hi) {
            int64_t rev = price[i] * (100 - disc[i]);
            total_sum += rev;
            promo_sum += rev * (int64_t)promo[partkey[i] - 1];
        }
    }
    total_sum_out = total_sum;
    promo_sum_out = promo_sum;
}

inline void run_q14_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q14_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // l_shipdate >= '1995-05-01' AND l_shipdate < '1995-06-01'
    const Date date_lo = date_to_epoch(1995, 5, 1);
    const Date date_hi = date_to_epoch(1995, 6, 1);

    // Build side of the hash join (part): collapse the p_type "PROMO" prefix
    // test into a dense byte array so the hot probe loop does a single cheap-
    // free lookup instead of a per-survivor std::string substring compare.
    const int32_t n_part = db->n_part;
    std::vector<uint8_t> is_promo(n_part);
    {
        PROFILE_SCOPE("q14_part_build");
        for (int32_t p = 0; p < n_part; p++) {
            const std::string& ptype = db->p_type[p];
            is_promo[p] = (ptype.size() >= 5 && std::memcmp(ptype.data(), "PROMO", 5) == 0) ? 1 : 0;
        }
    }

    // Exact integer accumulation: revenue = extendedprice(cents) * (100 - discount%).
    // Magnitudes (~1e9 per row * 12M rows ~ 1.2e16) stay within int64.
    int64_t promo_sum = 0;
    int64_t total_sum = 0;

    {
        PROFILE_SCOPE("q14_lineitem_scan_join_agg");
        const int64_t n = db->n_lineitem;
        q14_scan_avx2(n, db->l_shipdate.data(), db->l_partkey.data(),
                      db->l_extendedprice.data(), db->l_discount.data(),
                      is_promo.data(), date_lo, date_hi, total_sum, promo_sum);
        TRACE_ADD(li_scanned, n);
        TRACE_ADD(li_emitted, 0);
    }
    TRACE_COUNT("q14_rows_scanned", li_scanned);
    TRACE_COUNT("q14_rows_emitted", li_emitted);
    TRACE_COUNT("q14_agg_rows_in", li_emitted);

    double promo_revenue = (total_sum == 0) ? 0.0 : 100.0 * (double)promo_sum / (double)total_sum;

    PROFILE_SCOPE("q14_output");
    write_csv_header(out, {"promo_revenue"});
    write_csv_row(out, {fmt_decimal(promo_revenue, 15)});
    TRACE_COUNT("q14_query_output_rows", 1);
}
