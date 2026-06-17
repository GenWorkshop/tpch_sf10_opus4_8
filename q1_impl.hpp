#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <map>
#include <vector>
#include <cmath>

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

static void run_q1(Database* db, const std::string& run_nr) {
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
        int64_t i = 0;
        // Two independent accumulator tables break the read-modify-write
        // dependency chain when consecutive rows fall into the same group.
        for (; i + 1 < n; i += 2) {
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
            if (shipdate[i + 1] <= max_shipdate) {
                int key = (((unsigned char)returnflag[i + 1]) << 7) | (unsigned char)linestatus[i + 1];
                Agg& agg = table1[key];
                int64_t price = extprice[i + 1];
                int64_t disc = discount[i + 1];
                int64_t disc_price = price * (100 - disc);
                agg.sum_qty += quantity[i + 1];
                agg.sum_base_price += price;
                agg.sum_disc_price += disc_price;
                agg.sum_charge += disc_price * (100 + tax[i + 1]);
                agg.sum_discount += disc;
                agg.count++;
            }
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
