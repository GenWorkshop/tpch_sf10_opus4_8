#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <map>

// q12: Shipping modes - lineitem join orders
// Filter: l_shipmode IN ('MAIL','FOB'), l_commitdate < l_receiptdate,
//         l_shipdate < l_commitdate, l_receiptdate in [1997-01-01, 1998-01-01)
// Group by l_shipmode, count high/low priority orders
// Order by l_shipmode

static void run_q12(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1997, 1, 1);
    const int32_t date_end = to_days(1998, 1, 1);

    struct Agg { int64_t high = 0; int64_t low = 0; };
    std::map<std::string, Agg> groups;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q12_scan_join_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            const std::string& mode = db->l_shipmode[i];
            if (mode != "MAIL" && mode != "FOB") continue;
            if (db->l_commitdate[i] >= db->l_receiptdate[i]) continue;
            if (db->l_shipdate[i] >= db->l_commitdate[i]) continue;
            int32_t rd = db->l_receiptdate[i];
            if (rd < date_start || rd >= date_end) continue;
            TRACE_INC(rows_emitted);

            // Lookup order priority
            int32_t ok = db->l_orderkey[i];
            int32_t oidx = db->orderkey_to_idx[ok];
            if (oidx < 0) continue;
            TRACE_INC(join_rows_emitted);
            const std::string& prio = db->o_orderpriority[oidx];

            if (prio == "1-URGENT" || prio == "2-HIGH") {
                groups[mode].high++;
            } else {
                groups[mode].low++;
            }
        }
    }
    TRACE_COUNT("q12_rows_scanned", rows_scanned);
    TRACE_COUNT("q12_rows_emitted", rows_emitted);
    TRACE_COUNT("q12_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q12_groups_created", groups.size());

    {
    PROFILE_SCOPE("q12_output");
    std::ostringstream oss;
    write_csv_header(oss, {"l_shipmode","high_line_count","low_line_count"});
    for (auto& [mode, agg] : groups) {
        write_csv_row(oss, {
            csv_quote(mode),
            fmt_decimal((double)agg.high, 1),
            fmt_decimal((double)agg.low, 1)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q12_query_output_rows", groups.size());
    }
    TRACE_FLUSH();
}
