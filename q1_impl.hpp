#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <algorithm>
#include <map>
#include <immintrin.h>

// Convert yyyy-mm-dd to days since epoch (1970-01-01)
inline int32_t date_to_epoch(int y, int m, int d) {
    // Using the algorithm from Howard Hinnant
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

inline void __attribute__((target("avx2"))) run_q1_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q1_total");
    // l_shipdate <= '1998-12-01' - 100 days = '1998-08-23'
    const Date cutoff = date_to_epoch(1998, 8, 23);

    // Group by (returnflag, linestatus). DuckDB uses a PERFECT_HASH_GROUP_BY
    // here: both grouping columns are single uppercase chars, so a direct
    // flat array indexed by (rf-'A')*26 + (ls-'A') is a perfect hash and
    // avoids the per-row tree lookup of std::map. Iterating the array in
    // index order yields the required ORDER BY rf, ls.
    struct Agg {
        int64_t sum_qty = 0;        // scale 2
        int64_t sum_base_price = 0; // scale 2
        int64_t sum_disc_price = 0; // scale 4 (price*discount both scale 2 → product scale 4)
        int64_t sum_charge = 0;     // scale 6 (disc_price * tax, scale 4 * scale 2 → scale 6)
        int64_t sum_disc = 0;       // scale 2 (for avg)
        int64_t count = 0;
    };

    constexpr int N_SLOTS = 26 * 26;
    // Two independent accumulator banks: adjacent rows scatter into different
    // banks to break the read-modify-write dependency chain when consecutive
    // rows fall into the same group.  Merged after the scan.
    static thread_local Agg t0[N_SLOTS];
    static thread_local Agg t1[N_SLOTS];
    for (int s = 0; s < N_SLOTS; s++) { t0[s] = Agg{}; t1[s] = Agg{}; }

    const Date*    __restrict ship  = db->l_shipdate.data();
    const char*    __restrict rf    = db->l_returnflag.data();
    const char*    __restrict ls    = db->l_linestatus.data();
    const int64_t* __restrict qtyA  = db->l_quantity.data();
    const int64_t* __restrict prA   = db->l_extendedprice.data();
    const int64_t* __restrict dscA  = db->l_discount.data();
    const int64_t* __restrict taxA  = db->l_tax.data();

    {
        PROFILE_SCOPE("q1_scan_agg");
        const int64_t n = db->n_lineitem;

        // AVX2 vectorised scan: 4 rows per iteration.  256-bit loads raise
        // memory-level parallelism; the arithmetic (subtract, two multiplies,
        // masking) runs 4-wide.  l_extendedprice < 2^31 and the intermediate
        // disc_price < 2^31, so both products fit the signed 32x32->64
        // multiply (_mm256_mul_epi32).  Rows failing the date filter are
        // zero-masked, so they contribute nothing even though the (scalar)
        // scatter still routes them to their group slot.
        const __m256i v100  = _mm256_set1_epi64x(100);
        const __m128i vmax  = _mm_set1_epi32(cutoff);
        const __m128i vones = _mm_set1_epi32(-1);

        alignas(32) int64_t mqty[4], mbase[4], mdp[4], mchg[4], mdisc[4], mmask[4];

        int64_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m128i shp   = _mm_loadu_si128((const __m128i*)(ship + i));
            __m128i gt    = _mm_cmpgt_epi32(shp, vmax);            // shipdate > cutoff
            __m128i pass  = _mm_xor_si128(gt, vones);              // shipdate <= cutoff
            __m256i mask  = _mm256_cvtepi32_epi64(pass);           // 4x 64-bit 0/-1

            __m256i price = _mm256_loadu_si256((const __m256i*)(prA + i));
            __m256i disc  = _mm256_loadu_si256((const __m256i*)(dscA + i));
            __m256i tax_  = _mm256_loadu_si256((const __m256i*)(taxA + i));
            __m256i qty   = _mm256_loadu_si256((const __m256i*)(qtyA + i));

            __m256i one_minus_disc = _mm256_sub_epi64(v100, disc);
            __m256i one_plus_tax   = _mm256_add_epi64(v100, tax_);
            __m256i disc_price = _mm256_mul_epi32(price, one_minus_disc);
            __m256i charge     = _mm256_mul_epi32(disc_price, one_plus_tax);

            _mm256_store_si256((__m256i*)mqty,  _mm256_and_si256(qty, mask));
            _mm256_store_si256((__m256i*)mbase, _mm256_and_si256(price, mask));
            _mm256_store_si256((__m256i*)mdp,   _mm256_and_si256(disc_price, mask));
            _mm256_store_si256((__m256i*)mchg,  _mm256_and_si256(charge, mask));
            _mm256_store_si256((__m256i*)mdisc, _mm256_and_si256(disc, mask));
            _mm256_store_si256((__m256i*)mmask, mask);

            #define Q1_SCATTER(J, TBL)                                       \
                {                                                            \
                    int idx = (rf[i + (J)] - 'A') * 26 + (ls[i + (J)] - 'A');\
                    Agg& g = TBL[idx];                                       \
                    g.sum_qty        += mqty[J];                             \
                    g.sum_base_price += mbase[J];                            \
                    g.sum_disc_price += mdp[J];                              \
                    g.sum_charge     += mchg[J];                             \
                    g.sum_disc       += mdisc[J];                            \
                    g.count          += (mmask[J] & 1);                      \
                }
            Q1_SCATTER(0, t0)
            Q1_SCATTER(1, t1)
            Q1_SCATTER(2, t0)
            Q1_SCATTER(3, t1)
            #undef Q1_SCATTER
        }

        // Scalar tail.
        for (; i < n; i++) {
            if (ship[i] <= cutoff) {
                int idx = (rf[i] - 'A') * 26 + (ls[i] - 'A');
                Agg& g = t0[idx];
                int64_t price = prA[i];
                int64_t disc = dscA[i];
                int64_t disc_price = price * (100 - disc);
                g.sum_qty        += qtyA[i];
                g.sum_base_price += price;
                g.sum_disc_price += disc_price;
                g.sum_charge     += disc_price * (100 + taxA[i]);
                g.sum_disc       += disc;
                g.count++;
            }
        }
    }

    // Merge the two banks into t0.
    Agg* __restrict slots = t0;
    for (int s = 0; s < N_SLOTS; s++) {
        slots[s].sum_qty        += t1[s].sum_qty;
        slots[s].sum_base_price += t1[s].sum_base_price;
        slots[s].sum_disc_price += t1[s].sum_disc_price;
        slots[s].sum_charge     += t1[s].sum_charge;
        slots[s].sum_disc       += t1[s].sum_disc;
        slots[s].count          += t1[s].count;
    }

    PROFILE_SCOPE("q1_output");
    write_csv_header(out, {"l_returnflag","l_linestatus","sum_qty","sum_base_price",
                           "sum_disc_price","sum_charge","avg_qty","avg_price","avg_disc","count_order"});

    uint64_t n_groups = 0;
    for (int idx = 0; idx < N_SLOTS; idx++) {
        const Agg& g = slots[idx];
        if (g.count == 0) continue;
        n_groups++;
        std::pair<char,char> key{ char('A' + idx / 26), char('A' + idx % 26) };
        // sum_qty: scale 2
        std::string s_sum_qty = fmt_money(static_cast<long long>(g.sum_qty), 2);
        // sum_base_price: scale 2
        std::string s_sum_base_price = fmt_money(static_cast<long long>(g.sum_base_price), 2);
        // sum_disc_price: scale 4
        std::string s_sum_disc_price = fmt_money(static_cast<long long>(g.sum_disc_price), 4);
        // sum_charge: scale 6
        std::string s_sum_charge = fmt_money(static_cast<long long>(g.sum_charge), 6);
        // avgs derived from exact int sums (values stored at scale 2)
        double avg_qty = (double)g.sum_qty / g.count / 100.0;
        double avg_price = (double)g.sum_base_price / g.count / 100.0;
        double avg_disc = (double)g.sum_disc / g.count / 100.0;

        std::string rf_s(1, key.first);
        std::string ls_s(1, key.second);

        write_csv_row(out, {
            rf_s,
            ls_s,
            s_sum_qty,
            s_sum_base_price,
            s_sum_disc_price,
            s_sum_charge,
            fmt_decimal(avg_qty, 15),
            fmt_decimal(avg_price, 15),
            fmt_decimal(avg_disc, 15),
            std::to_string(g.count)
        });
    }
    TRACE_COUNT("q1_groups_created", n_groups);
    TRACE_COUNT("q1_agg_rows_emitted", n_groups);
    TRACE_COUNT("q1_query_output_rows", n_groups);
}
