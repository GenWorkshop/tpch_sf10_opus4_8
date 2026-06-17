#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <immintrin.h>

inline void __attribute__((target("avx2"))) run_q6_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q6_total");
    // l_shipdate >= '1993-01-01' AND l_shipdate < '1994-01-01'
    // l_discount BETWEEN 0.07 AND 0.09 (stored as scale 2: 7..9)
    // l_quantity < 24 (stored as scale 2: 2400)
    const Date date_lo = date_to_epoch(1993, 1, 1);
    const Date date_hi = date_to_epoch(1994, 1, 1);
    const int64_t disc_lo = 7;
    const int64_t disc_hi = 9;
    const int64_t qty_limit = 2400;

    // sum(l_extendedprice * l_discount): extendedprice < 2^31 cents and
    // discount <= 9, so each product < 2^31 and the total (~12M passing rows)
    // stays well within int64. No __int128 needed.
    int64_t revenue = 0;

    TRACE_DECL_COUNTER(rows_scanned);
    TRACE_DECL_COUNTER(rows_emitted);

    const Date* __restrict shipdate = db->l_shipdate.data();
    const int64_t* __restrict disc = db->l_discount.data();
    const int64_t* __restrict qty = db->l_quantity.data();
    const int64_t* __restrict eprice = db->l_extendedprice.data();
    const int64_t n = db->n_lineitem;

    {
        PROFILE_SCOPE("q6_scan_filter_agg");

        const uint32_t drange = (uint32_t)(date_hi - date_lo);
        const __m128i vdlo = _mm_set1_epi32(date_lo);
        const __m128i vsign32 = _mm_set1_epi32((int)0x80000000);
        const __m128i vdrange_x = _mm_set1_epi32((int)(drange ^ 0x80000000u));

        // discount: (uint64)(disc - 7) <= 2  <=>  (uint64)(disc - 7) < 3
        const __m256i vdisc_lo = _mm256_set1_epi64x(disc_lo);
        const __m256i vsign64 = _mm256_set1_epi64x((long long)0x8000000000000000ULL);
        const __m256i vdisc_range_x =
            _mm256_set1_epi64x((long long)(3ULL ^ 0x8000000000000000ULL));
        const __m256i vqty_limit = _mm256_set1_epi64x(qty_limit);

        __m256i vacc = _mm256_setzero_si256();

        int64_t i = 0;
        const int64_t n4 = n & ~(int64_t)3;
        for (; i < n4; i += 4) {
            // shipdate range check (4x int32 -> 4x int32 mask)
            __m128i d = _mm_loadu_si128((const __m128i*)(shipdate + i));
            __m128i dsub = _mm_sub_epi32(d, vdlo);
            __m128i dsub_x = _mm_xor_si128(dsub, vsign32);
            __m128i dmask32 = _mm_cmpgt_epi32(vdrange_x, dsub_x);
            __m256i dmask = _mm256_cvtepi32_epi64(dmask32);

            // discount range check (4x int64)
            __m256i vd = _mm256_loadu_si256((const __m256i*)(disc + i));
            __m256i dcsub = _mm256_sub_epi64(vd, vdisc_lo);
            __m256i dcsub_x = _mm256_xor_si256(dcsub, vsign64);
            __m256i dcmask = _mm256_cmpgt_epi64(vdisc_range_x, dcsub_x);

            // quantity < 2400 (4x int64, signed/positive)
            __m256i vq = _mm256_loadu_si256((const __m256i*)(qty + i));
            __m256i qmask = _mm256_cmpgt_epi64(vqty_limit, vq);

            __m256i mask = _mm256_and_si256(_mm256_and_si256(dmask, dcmask), qmask);

            // products: low 32 bits of extendedprice * low 32 bits of discount
            __m256i ve = _mm256_loadu_si256((const __m256i*)(eprice + i));
            __m256i prod = _mm256_mul_epi32(ve, vd);
            prod = _mm256_and_si256(prod, mask);
            vacc = _mm256_add_epi64(vacc, prod);

#ifdef TRACE
            rows_emitted += (int64_t)__builtin_popcount(
                (unsigned)_mm256_movemask_pd(_mm256_castsi256_pd(mask)));
#endif
        }

        // horizontal sum of the 4 int64 lanes
        int64_t lanes[4];
        _mm256_storeu_si256((__m256i*)lanes, vacc);
        revenue += lanes[0] + lanes[1] + lanes[2] + lanes[3];

        // scalar tail
        for (; i < n; i++) {
            if (shipdate[i] >= date_lo && shipdate[i] < date_hi &&
                disc[i] >= disc_lo && disc[i] <= disc_hi &&
                qty[i] < qty_limit) {
                TRACE_INC(rows_emitted);
                revenue += eprice[i] * disc[i];
            }
        }
        TRACE_ADD(rows_scanned, n);
    }

    TRACE_COUNT("q6_rows_scanned", rows_scanned);
    TRACE_COUNT("q6_rows_emitted", rows_emitted);
    TRACE_COUNT("q6_agg_rows_in", rows_emitted);

    PROFILE_SCOPE("q6_output");
    write_csv_header(out, {"revenue"});
    write_csv_row(out, {fmt_money(static_cast<long long>(revenue), 4)});
    TRACE_COUNT("q6_query_output_rows", 1);
}
