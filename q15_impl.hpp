#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <vector>

// q15: Top supplier - CTE computes revenue per supplier for shipdate in [1996-02-01, 1996-05-01)
// Then find supplier(s) with max total_revenue
// Order by s_suppkey

static void run_q15(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1996, 2, 1);
    const int32_t date_end = to_days(1996, 5, 1);

    // Compute revenue per supplier: sum(l_extendedprice * (1 - l_discount)) scale 4
    std::unordered_map<int32_t, int64_t> supp_revenue;
    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q15_lineitem_scan_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t sd = db->l_shipdate[i];
            if (sd >= date_start && sd < date_end) {
                TRACE_INC(rows_emitted);
                int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]);
                supp_revenue[db->l_suppkey[i]] += rev;
            }
        }
    }
    TRACE_COUNT("q15_rows_scanned", rows_scanned);
    TRACE_COUNT("q15_rows_emitted", rows_emitted);
    TRACE_COUNT("q15_groups_created", supp_revenue.size());

    // Find max revenue
    int64_t max_rev = 0;
    {
        PROFILE_SCOPE("q15_max");
        for (auto& [sk, rev] : supp_revenue) {
            if (rev > max_rev) max_rev = rev;
        }
    }

    // Collect suppliers with max revenue
    struct Result { int32_t suppkey; };
    std::vector<Result> results;
    for (auto& [sk, rev] : supp_revenue) {
        if (rev == max_rev) {
            results.push_back({sk});
        }
    }

    {
        PROFILE_SCOPE("q15_sort");
        // Sort by s_suppkey
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            return a.suppkey < b.suppkey;
        });
    }
    TRACE_COUNT("q15_sort_rows_in", results.size());
    TRACE_COUNT("q15_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q15_output");
    std::ostringstream oss;
    write_csv_header(oss, {"s_suppkey","s_name","s_address","s_phone","total_revenue"});
    for (auto& r : results) {
        int32_t sk = r.suppkey;
        write_csv_row(oss, {
            std::to_string(sk),
            csv_quote(db->s_name[sk]),
            csv_quote(db->s_address[sk]),
            csv_quote(db->s_phone[sk]),
            fmt_decimal(max_rev / 10000.0, 4)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q15_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
