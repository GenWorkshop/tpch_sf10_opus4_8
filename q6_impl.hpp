#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <sstream>
#include <fstream>

// q6: sum(l_extendedprice * l_discount) where shipdate in [1993-01-01, 1994-01-01)
// and discount between 0.07 and 0.09, and quantity < 24
// Revenue output is scale 4 (price scale2 * discount scale2)

static void run_q6(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1993, 1, 1);
    const int32_t date_end = to_days(1994, 1, 1);
    // discount between 0.07 and 0.09 -> in scale2: between 7 and 9
    const int64_t disc_lo = 7;
    const int64_t disc_hi = 9;
    // quantity < 24 -> in scale2: < 2400
    const int64_t qty_max = 2400;

    __int128 revenue = 0; // scale 4
    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q6_scan");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t sd = db->l_shipdate[i];
            if (sd >= date_start && sd < date_end) {
                int64_t disc = db->l_discount[i];
                if (disc >= disc_lo && disc <= disc_hi) {
                    if (db->l_quantity[i] < qty_max) {
                        TRACE_INC(rows_emitted);
                        revenue += (__int128)db->l_extendedprice[i] * disc;
                    }
                }
            }
        }
    }
    TRACE_COUNT("q6_rows_scanned", rows_scanned);
    TRACE_COUNT("q6_rows_emitted", rows_emitted);

    double rev_d = static_cast<double>(static_cast<long long>(revenue)) / 10000.0;

    {
    PROFILE_SCOPE("q6_output");
    std::ostringstream oss;
    write_csv_header(oss, {"revenue"});
    write_csv_row(oss, {fmt_decimal(rev_d, 4)});

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q6_query_output_rows", 1);
    }
    TRACE_FLUSH();
}
