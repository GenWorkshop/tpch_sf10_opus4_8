#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <map>
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
    struct Agg {
        __int128 sum_qty = 0;        // sum of l_quantity (in cents, scale 2)
        __int128 sum_base_price = 0; // sum of l_extendedprice (scale 2)
        __int128 sum_disc_price = 0; // sum of price*(1-discount) (scale 4)
        __int128 sum_charge = 0;     // sum of price*(1-disc)*(1+tax) (scale 6)
        __int128 sum_discount = 0;   // for avg (scale 2)
        int64_t count = 0;
    };

    // Use ordered map with key = (returnflag, linestatus) for sorted output
    std::map<std::pair<char,char>, Agg> groups;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q1_scan_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            if (db->l_shipdate[i] <= max_shipdate) {
                TRACE_INC(rows_emitted);
                char rf = db->l_returnflag[i];
            char ls = db->l_linestatus[i];
            auto& agg = groups[{rf, ls}];

            int64_t qty = db->l_quantity[i];       // scale 2
            int64_t price = db->l_extendedprice[i]; // scale 2
            int64_t disc = db->l_discount[i];       // scale 2
            int64_t tax = db->l_tax[i];             // scale 2

            agg.sum_qty += qty;
            agg.sum_base_price += price;
            // disc_price = price * (1 - disc/100) = price * (100 - disc) / 100
            // But scale: price is scale2, disc is scale2
            // price*(1-discount): price(scale2) * (100 - disc)(scale2 value representing 1-d)
            // Actually: l_discount stored as scale2, so 0.05 = 5
            // 1 - l_discount in raw: 100 - disc_raw (since 1.00 = 100 in scale2)
            // price * (100 - disc) gives scale4 result
            __int128 disc_price = (__int128)price * (100 - disc);
            agg.sum_disc_price += disc_price;
            // charge = disc_price * (1 + tax) = disc_price * (100 + tax) / 100
            // disc_price is scale4, (100+tax) is scale2 magnitude -> result scale6
            __int128 charge = disc_price * (100 + tax);
            agg.sum_charge += charge;
            agg.sum_discount += disc;
            agg.count++;
        }
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
