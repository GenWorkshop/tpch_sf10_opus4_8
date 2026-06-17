#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

inline void run_q5_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q5_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // r_name = 'AFRICA' → regionkey
    int32_t target_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "AFRICA") { target_regionkey = i; break; }
    }

    // Nations in target region
    std::unordered_set<int32_t> target_nations;
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == target_regionkey) {
            target_nations.insert(i);
        }
    }

    // Date filter: o_orderdate >= '1997-01-01' AND < '1998-01-01'
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    // Find qualifying orders: orderdate in range AND customer's nation in target region
    // Store order_idx → customer nationkey (for later join condition c_nationkey = s_nationkey)
    std::vector<int32_t> order_cust_nationkey(db->n_orders, -1); // -1 = not qualifying
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
            int32_t custkey = db->o_custkey[i];
            int32_t c_idx = custkey - 1;
            if (c_idx >= 0 && c_idx < db->n_customer) {
                int32_t c_nation = db->c_nationkey[c_idx];
                if (target_nations.count(c_nation)) {
                    order_cust_nationkey[i] = c_nation;
                }
            }
        }
    }

    // Scan lineitem, join with qualifying orders, check s_nationkey = c_nationkey
    // Group by nation name → sum revenue
    std::unordered_map<int32_t, int64_t> nation_revenue; // nationkey → sum of revenue (scale 4)

    {
        PROFILE_SCOPE("q5_lineitem_scan_join_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            int32_t orderkey = db->l_orderkey[i];
            if (orderkey > db->max_orderkey) continue;
            int32_t o_idx = db->orderkey_to_idx[orderkey];
            if (o_idx < 0) continue;
            int32_t c_nation = order_cust_nationkey[o_idx];
            if (c_nation < 0) continue;

            // Check s_nationkey = c_nationkey
            int32_t suppkey = db->l_suppkey[i];
            int32_t s_idx = suppkey - 1;
            if (s_idx >= 0 && s_idx < db->n_supplier && db->s_nationkey[s_idx] == c_nation) {
                TRACE_INC(li_emitted);
                int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]); // scale 4
                nation_revenue[c_nation] += rev;
            }
        }
    }
    TRACE_COUNT("q5_rows_scanned", li_scanned);
    TRACE_COUNT("q5_join_rows_emitted", li_emitted);
    TRACE_COUNT("q5_agg_rows_in", li_emitted);
    TRACE_COUNT("q5_groups_created", (uint64_t)nation_revenue.size());
    TRACE_COUNT("q5_agg_rows_emitted", (uint64_t)nation_revenue.size());

    // Sort by revenue desc
    struct ResultRow {
        std::string n_name;
        int64_t revenue;
    };
    std::vector<ResultRow> results;
    for (auto& [nk, rev] : nation_revenue) {
        results.push_back({db->n_name[nk], rev});
    }
    TRACE_COUNT("q5_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q5_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q5_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q5_output");
    write_csv_header(out, {"n_name","revenue"});
    for (auto& r : results) {
        write_csv_row(out, {r.n_name, fmt_money(r.revenue, 4)});
    }
    TRACE_COUNT("q5_query_output_rows", (uint64_t)results.size());
}
