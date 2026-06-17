#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <string>

inline bool matches_express_requests(const std::string& s) {
    // Match pattern '%express%requests%'
    auto pos = s.find("express");
    if (pos == std::string::npos) return false;
    return s.find("requests", pos + 7) != std::string::npos;
}

inline void run_q13_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q13_total");
    TRACE_DECL_COUNTER(orders_scanned);
    TRACE_DECL_COUNTER(orders_emitted);
    // For each customer, count orders where o_comment NOT LIKE '%express%requests%'
    // LEFT OUTER JOIN means customers with 0 qualifying orders get c_count = 0

    // Count orders per customer
    std::vector<int32_t> cust_order_count(db->n_customer, 0);

    {
        PROFILE_SCOPE("q13_orders_scan_join");
        for (int32_t i = 0; i < db->n_orders; i++) {
            TRACE_INC(orders_scanned);
            if (!matches_express_requests(db->o_comment[i])) {
                TRACE_INC(orders_emitted);
                int32_t custkey = db->o_custkey[i];
                int32_t c_idx = custkey - 1;
                if (c_idx >= 0 && c_idx < db->n_customer) {
                    cust_order_count[c_idx]++;
                }
            }
        }
    }

    // Build histogram: c_count → number of customers with that count
    std::unordered_map<int32_t, int64_t> histogram;
    {
        PROFILE_SCOPE("q13_histogram_agg");
        for (int32_t i = 0; i < db->n_customer; i++) {
            histogram[cust_order_count[i]]++;
        }
    }
    TRACE_COUNT("q13_rows_scanned", orders_scanned);
    TRACE_COUNT("q13_rows_emitted", orders_emitted);
    TRACE_COUNT("q13_agg_rows_in", (uint64_t)db->n_customer);
    TRACE_COUNT("q13_groups_created", (uint64_t)histogram.size());
    TRACE_COUNT("q13_agg_rows_emitted", (uint64_t)histogram.size());

    // Sort by custdist desc, c_count desc
    struct ResultRow {
        int32_t c_count;
        int64_t custdist;
    };
    std::vector<ResultRow> results;
    results.reserve(histogram.size());
    for (auto& [cnt, dist] : histogram) {
        results.push_back({cnt, dist});
    }
    TRACE_COUNT("q13_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q13_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.custdist != b.custdist) return a.custdist > b.custdist;
            return a.c_count > b.c_count;
        });
    }
    TRACE_COUNT("q13_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q13_output");
    write_csv_header(out, {"c_count","custdist"});
    for (auto& r : results) {
        write_csv_row(out, {
            std::to_string(r.c_count),
            std::to_string(r.custdist)
        });
    }
    TRACE_COUNT("q13_query_output_rows", (uint64_t)results.size());
}
