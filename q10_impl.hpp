#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>

inline void run_q10_impl(Database* db, std::ostream& out) {
    // o_orderdate >= '1993-08-01' AND o_orderdate < '1993-11-01'
    const Date date_lo = date_to_epoch(1993, 8, 1);
    const Date date_hi = date_to_epoch(1993, 11, 1);

    // Find qualifying orders and map orderkey → custkey
    std::vector<bool> order_qualifies(db->n_orders, false);
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
            order_qualifies[i] = true;
        }
    }

    // Scan lineitem: filter returnflag='R', join with qualifying orders
    // Group by custkey → sum revenue
    std::unordered_map<int32_t, int64_t> cust_revenue; // custkey → revenue (scale 4)

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        if (db->l_returnflag[i] != 'R') continue;

        int32_t orderkey = db->l_orderkey[i];
        if (orderkey > db->max_orderkey) continue;
        int32_t o_idx = db->orderkey_to_idx[orderkey];
        if (o_idx < 0 || !order_qualifies[o_idx]) continue;

        int32_t custkey = db->o_custkey[o_idx];
        int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]); // scale 4
        cust_revenue[custkey] += rev;
    }

    // Build result rows
    struct ResultRow {
        int32_t c_custkey;
        int64_t revenue;
    };
    std::vector<ResultRow> results;
    results.reserve(cust_revenue.size());
    for (auto& [ck, rev] : cust_revenue) {
        results.push_back({ck, rev});
    }

    // Order by revenue desc
    std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
        return a.revenue > b.revenue;
    });

    write_csv_header(out, {"c_custkey","c_name","revenue","c_acctbal","n_name","c_address","c_phone","c_comment"});
    for (auto& r : results) {
        int32_t c_idx = r.c_custkey - 1;
        int32_t nk = db->c_nationkey[c_idx];
        write_csv_row(out, {
            std::to_string(r.c_custkey),
            csv_quote(db->c_name[c_idx]),
            fmt_money(r.revenue, 4),
            fmt_money(db->c_acctbal[c_idx], 2),
            csv_quote(db->n_name[nk]),
            csv_quote(db->c_address[c_idx]),
            csv_quote(db->c_phone[c_idx]),
            csv_quote(db->c_comment[c_idx])
        });
    }
}
