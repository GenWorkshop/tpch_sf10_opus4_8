#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <string>
#include <cstring>

inline bool matches_express_requests(const std::string& s) {
    // Match pattern '%express%requests%'.
    // Scan for the rare anchor chars 'x' (in expre[x]s) and 'q' (in re[q]uests)
    // rather than the common 'e' to minimize candidate verifications.
    const char* d = s.data();
    size_t len = s.size();
    const char* end = d + len;
    const char* p = static_cast<const char*>(memchr(d, 'x', len));
    while (p) {
        if (p - d >= 1 && end - p >= 6 && std::memcmp(p - 1, "express", 7) == 0) {
            const char* after = p + 6;
            const char* q = static_cast<const char*>(memchr(after, 'q', end - after));
            while (q) {
                if (q - d >= 2 && end - q >= 6 && std::memcmp(q - 2, "requests", 8) == 0)
                    return true;
                q = static_cast<const char*>(memchr(q + 1, 'q', end - (q + 1)));
            }
            return false;
        }
        p = static_cast<const char*>(memchr(p + 1, 'x', end - (p + 1)));
    }
    return false;
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
        const std::string* comments = db->o_comment.data();
        const int32_t* custkeys = db->o_custkey.data();
        const int32_t n = db->n_orders;
        constexpr int PFD = 16;
        for (int32_t i = 0; i < n; i++) {
            TRACE_INC(orders_scanned);
            if (i + PFD < n) {
                // Prefetch the heap char buffer of the comment PFD iterations ahead.
                const std::string& fs = comments[i + PFD];
                __builtin_prefetch(fs.data(), 0, 0);
                __builtin_prefetch(&custkeys[i + PFD], 0, 1);
            }
            const std::string& cs = comments[i];
            if (!matches_express_requests(cs)) {
                TRACE_INC(orders_emitted);
                int32_t custkey = custkeys[i];
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
