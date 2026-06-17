#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <map>
#include <vector>
#include <cmath>
#include <immintrin.h>

// q1: lineitem scan with date filter, group by returnflag+linestatus, aggregate
// l_shipdate <= date '1998-12-01' - interval '100' day = 1998-08-23
// Date epoch for Arrow is 1970-01-01. 1998-08-23 = days since epoch.

static inline int32_t date_to_days(int y, int m, int d) {
    // Days since 1970-01-01 using the civil calendar algorithm
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

static void __attribute__((target("avx2"))) run_q1(Database* db, const std::string& run_nr) {
    // 1998-12-01 minus 100 days = 1998-08-23
    const int32_t max_shipdate = date_to_days(1998, 8, 23);

    // Group key: (returnflag, linestatus) -> 2 chars
    // All sums fit comfortably in int64 even at large scale factors
    // (sum_charge worst case ~5e16 << 9.2e18), so we avoid __int128 entirely.
    struct Agg {
        int64_t sum_qty = 0;        // scale 2
        int64_t sum_base_price = 0; // scale 2
        int64_t sum_disc_price = 0; // scale 4
        int64_t sum_charge = 0;     // scale 6
        int64_t sum_discount = 0;   // scale 2
        int64_t count = 0;
    };

    // Perfect-hash group-by: group count is tiny (returnflag x linestatus).
    // Index directly by (returnflag<<7 | linestatus) into a flat array to avoid
    // per-row tree/hash lookups over the full lineitem scan.
    constexpr int IDX = 128 * 128;
    static thread_local std::vector<Agg> table0v, table1v;
    table0v.assign(IDX, Agg{});
    table1v.assign(IDX, Agg{});
    Agg* __restrict table0 = table0v.data();
    Agg* __restrict table1 = table1v.data();

    const int32_t* __restrict shipdate = db->l_shipdate.data();
    const char* __restrict returnflag = db->l_returnflag.data();
    const char* __restrict linestatus = db->l_linestatus.data();
    const int64_t* __restrict quantity = db->l_quantity.data();
    const int64_t* __restrict extprice = db->l_extendedprice.data();
    const int64_t* __restrict discount = db->l_discount.data();
    const int64_t* __restrict tax = db->l_tax.data();

    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q1_scan_agg");
        const int64_t n = db->lineitem_count;

        // AVX2 vectorised scan: process 4 rows per iteration.  256-bit loads
        // raise memory-level parallelism and the arithmetic (subtract, the two
        // multiplies, masking) runs 4-wide.  l_extendedprice < 2^31 and the
        // intermediate disc_price < 2^31, so the products fit the signed
        // 32x32 -> 64 multiply (_mm256_mul_epi32).  The grouped scatter stays
        // scalar but is fed from masked vectors, with adjacent rows routed to
        // two independent tables to break the read-modify-write chain.
        const __m256i v100 = _mm256_set1_epi64x(100);
        const __m128i vmax = _mm_set1_epi32(max_shipdate);
        const __m128i vones = _mm_set1_epi32(-1);

        alignas(32) int64_t mqty[4], mbase[4], mdisc_price[4], mcharge[4], mdisc[4], mmask[4];

        int64_t i = 0;
        for (; i + 4 <= n; i += 4) {
            __m128i ship = _mm_loadu_si128((const __m128i*)(shipdate + i));
            // pass = shipdate <= max  ==  !(shipdate > max)
            __m128i gt = _mm_cmpgt_epi32(ship, vmax);
            __m128i pass32 = _mm_xor_si128(gt, vones);
            __m256i mask = _mm256_cvtepi32_epi64(pass32);   // 4x 64-bit 0/-1

            __m256i qty   = _mm256_loadu_si256((const __m256i*)(quantity + i));
            __m256i price = _mm256_loadu_si256((const __m256i*)(extprice + i));
            __m256i disc  = _mm256_loadu_si256((const __m256i*)(discount + i));
            __m256i tax_  = _mm256_loadu_si256((const __m256i*)(tax + i));

            __m256i one_minus_disc = _mm256_sub_epi64(v100, disc);     // 100 - disc
            __m256i one_plus_tax   = _mm256_add_epi64(v100, tax_);     // 100 + tax
            __m256i disc_price = _mm256_mul_epi32(price, one_minus_disc);
            __m256i charge     = _mm256_mul_epi32(disc_price, one_plus_tax);

            _mm256_store_si256((__m256i*)mqty,        _mm256_and_si256(qty, mask));
            _mm256_store_si256((__m256i*)mbase,       _mm256_and_si256(price, mask));
            _mm256_store_si256((__m256i*)mdisc_price, _mm256_and_si256(disc_price, mask));
            _mm256_store_si256((__m256i*)mcharge,     _mm256_and_si256(charge, mask));
            _mm256_store_si256((__m256i*)mdisc,       _mm256_and_si256(disc, mask));
            _mm256_store_si256((__m256i*)mmask,       mask);

            #define Q1_SCATTER(J, TBL)                                                       \
                {                                                                            \
                    int key = (((unsigned char)returnflag[i + (J)]) << 7)                    \
                              | (unsigned char)linestatus[i + (J)];                          \
                    Agg& agg = TBL[key];                                                     \
                    agg.sum_qty += mqty[J];                                                  \
                    agg.sum_base_price += mbase[J];                                          \
                    agg.sum_disc_price += mdisc_price[J];                                    \
                    agg.sum_charge += mcharge[J];                                            \
                    agg.sum_discount += mdisc[J];                                            \
                    agg.count += (mmask[J] & 1);                                             \
                }
            Q1_SCATTER(0, table0)
            Q1_SCATTER(1, table1)
            Q1_SCATTER(2, table0)
            Q1_SCATTER(3, table1)
            #undef Q1_SCATTER
        }
        for (; i < n; i++) {
            if (shipdate[i] <= max_shipdate) {
                int key = (((unsigned char)returnflag[i]) << 7) | (unsigned char)linestatus[i];
                Agg& agg = table0[key];
                int64_t price = extprice[i];
                int64_t disc = discount[i];
                int64_t disc_price = price * (100 - disc);
                agg.sum_qty += quantity[i];
                agg.sum_base_price += price;
                agg.sum_disc_price += disc_price;
                agg.sum_charge += disc_price * (100 + tax[i]);
                agg.sum_discount += disc;
                agg.count++;
            }
        }
    }

    // Merge the two tables and collect populated groups in sorted order.
    std::vector<std::pair<std::pair<char,char>, Agg>> groups;
    for (int k = 0; k < IDX; k++) {
        const Agg& a0 = table0[k];
        const Agg& a1 = table1[k];
        if (a0.count || a1.count) {
            Agg m;
            m.sum_qty = a0.sum_qty + a1.sum_qty;
            m.sum_base_price = a0.sum_base_price + a1.sum_base_price;
            m.sum_disc_price = a0.sum_disc_price + a1.sum_disc_price;
            m.sum_charge = a0.sum_charge + a1.sum_charge;
            m.sum_discount = a0.sum_discount + a1.sum_discount;
            m.count = a0.count + a1.count;
            char rf = (char)(k >> 7);
            char ls = (char)(k & 127);
            groups.push_back({{rf, ls}, m});
        }
    }
    TRACE_COUNT("q1_rows_scanned", rows_scanned);
    TRACE_COUNT("q1_rows_emitted", rows_emitted);
    TRACE_COUNT("q1_agg_rows_in", rows_emitted);
    TRACE_COUNT("q1_groups_created", groups.size());
    TRACE_COUNT("q1_agg_rows_emitted", groups.size());

    {
    PROFILE_SCOPE("q1_output");
    std::ostringstream oss;
    write_csv_header(oss, {"l_returnflag","l_linestatus","sum_qty","sum_base_price",
                           "sum_disc_price","sum_charge","avg_qty","avg_price","avg_disc","count_order"});

    for (auto& [key, agg] : groups) {
        std::string rf(1, key.first);
        std::string ls(1, key.second);

        // sum_qty: scale 2
        double sum_qty_d = static_cast<double>(static_cast<long long>(agg.sum_qty)) / 100.0;
        // sum_base_price: scale 2
        double sum_base_d = static_cast<double>(static_cast<long long>(agg.sum_base_price)) / 100.0;
        // sum_disc_price: scale 4
        double sum_disc_d = static_cast<double>(static_cast<long long>(agg.sum_disc_price)) / 10000.0;
        // sum_charge: scale 6
        double sum_charge_d = static_cast<double>(static_cast<long long>(agg.sum_charge)) / 1000000.0;

        // avg_qty = sum_qty / count (in original units, so sum_qty_d / count)
        double avg_qty = sum_qty_d / agg.count;
        // avg_price = sum_base_price / count
        double avg_price = sum_base_d / agg.count;
        // avg_disc = sum_discount / count (scale 2)
        double avg_disc = static_cast<double>(static_cast<long long>(agg.sum_discount)) / 100.0 / agg.count;

        write_csv_row(oss, {
            rf, ls,
            fmt_decimal(sum_qty_d, 2),
            fmt_decimal(sum_base_d, 2),
            fmt_decimal(sum_disc_d, 4),
            fmt_decimal(sum_charge_d, 6),
            fmt_decimal(avg_qty, 15),  // DOUBLE - use high precision
            fmt_decimal(avg_price, 15),
            fmt_decimal(avg_disc, 15),
            std::to_string(agg.count)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q1_query_output_rows", groups.size());
    }
    TRACE_FLUSH();
}
