#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_set>

// q4: orders in date range [1993-09-01, 1993-12-01) where EXISTS lineitem
// with l_commitdate < l_receiptdate, group by o_orderpriority, order by priority

static void run_q4(Database* db, const std::string& run_nr) {
    // 1993-09-01 and 1993-12-01
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1993, 9, 1);
    const int32_t date_end = to_days(1993, 12, 1);

    // Build set of orderkeys that have at least one lineitem with commitdate < receiptdate
    std::unordered_set<int32_t> late_orderkeys;
    TRACE_DECL(build_rows_in);
    {
        PROFILE_SCOPE("q4_semijoin_build");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(build_rows_in);
            if (db->l_commitdate[i] < db->l_receiptdate[i]) {
                late_orderkeys.insert(db->l_orderkey[i]);
            }
        }
    }
    TRACE_COUNT("q4_build_rows_in", build_rows_in);
    TRACE_COUNT("q4_build_rows", late_orderkeys.size());

    // Scan orders in date range with EXISTS condition
    std::map<std::string, int64_t> counts;
    TRACE_DECL(probe_rows_in);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q4_scan_probe_agg");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(probe_rows_in);
            int32_t od = db->o_orderdate[i];
            if (od >= date_start && od < date_end) {
                if (late_orderkeys.count(db->o_orderkey[i])) {
                    TRACE_INC(join_rows_emitted);
                    counts[db->o_orderpriority[i]]++;
                }
            }
        }
    }
    TRACE_COUNT("q4_probe_rows_in", probe_rows_in);
    TRACE_COUNT("q4_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q4_groups_created", counts.size());

    {
    PROFILE_SCOPE("q4_output");
    std::ostringstream oss;
    write_csv_header(oss, {"o_orderpriority","order_count"});
    for (auto& [prio, cnt] : counts) {
        write_csv_row(oss, {prio, std::to_string(cnt)});
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q4_query_output_rows", counts.size());
    }
    TRACE_FLUSH();
}
