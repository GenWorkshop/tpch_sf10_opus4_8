#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>

inline void run_q15_impl(Database* db, std::ostream& out) {
    // CTE revenue: l_shipdate >= '1996-02-01' AND l_shipdate < '1996-05-01'
    const Date date_lo = date_to_epoch(1996, 2, 1);
    const Date date_hi = date_to_epoch(1996, 5, 1);

    // Compute total_revenue per supplier
    std::unordered_map<int32_t, int64_t> supp_revenue; // suppkey → revenue (scale 4)

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        if (db->l_shipdate[i] >= date_lo && db->l_shipdate[i] < date_hi) {
            int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]);
            supp_revenue[db->l_suppkey[i]] += rev;
        }
    }

    // Find max revenue
    int64_t max_rev = 0;
    for (auto& [sk, rev] : supp_revenue) {
        if (rev > max_rev) max_rev = rev;
    }

    // Collect suppliers with max revenue
    struct ResultRow {
        int32_t s_suppkey;
        int64_t total_revenue;
    };
    std::vector<ResultRow> results;
    for (auto& [sk, rev] : supp_revenue) {
        if (rev == max_rev) {
            results.push_back({sk, rev});
        }
    }

    // Order by s_suppkey
    std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
        return a.s_suppkey < b.s_suppkey;
    });

    write_csv_header(out, {"s_suppkey","s_name","s_address","s_phone","total_revenue"});
    for (auto& r : results) {
        int32_t s_idx = r.s_suppkey - 1;
        write_csv_row(out, {
            std::to_string(r.s_suppkey),
            csv_quote(db->s_name[s_idx]),
            csv_quote(db->s_address[s_idx]),
            csv_quote(db->s_phone[s_idx]),
            fmt_money(r.total_revenue, 4)
        });
    }
}
