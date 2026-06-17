#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q3_impl.hpp" // format_date
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

inline void run_q18_impl(Database* db, std::ostream& out) {
    // First: find orderkeys where sum(l_quantity) > 314 (scale 2: 31400)
    const int64_t qty_threshold = 31400; // 314 * 100

    // Compute sum(l_quantity) per orderkey
    std::unordered_map<int32_t, int64_t> order_qty_sum;
    for (int64_t i = 0; i < db->n_lineitem; i++) {
        order_qty_sum[db->l_orderkey[i]] += db->l_quantity[i];
    }

    // Filter orders with sum > threshold
    std::unordered_set<int32_t> big_orders;
    for (auto& [ok, sum] : order_qty_sum) {
        if (sum > qty_threshold) {
            big_orders.insert(ok);
        }
    }

    if (big_orders.empty()) {
        write_csv_header(out, {"c_name","c_custkey","o_orderkey","o_orderdate","o_totalprice","sum(l_quantity)"});
        return;
    }

    // For qualifying orders, compute sum(l_quantity) per order from lineitem
    // (already have it in order_qty_sum)
    // Build result rows
    struct ResultRow {
        std::string c_name;
        int32_t c_custkey;
        int32_t o_orderkey;
        Date o_orderdate;
        int64_t o_totalprice; // scale 2
        int64_t sum_qty;      // scale 2
    };
    std::vector<ResultRow> results;

    for (int32_t ok : big_orders) {
        if (ok > db->max_orderkey) continue;
        int32_t o_idx = db->orderkey_to_idx[ok];
        if (o_idx < 0) continue;

        int32_t custkey = db->o_custkey[o_idx];
        int32_t c_idx = custkey - 1;

        results.push_back({
            db->c_name[c_idx],
            custkey,
            ok,
            db->o_orderdate[o_idx],
            db->o_totalprice[o_idx],
            order_qty_sum[ok]
        });
    }

    // Order by o_totalprice desc, o_orderdate
    std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
        if (a.o_totalprice != b.o_totalprice) return a.o_totalprice > b.o_totalprice;
        return a.o_orderdate < b.o_orderdate;
    });

    write_csv_header(out, {"c_name","c_custkey","o_orderkey","o_orderdate","o_totalprice","sum(l_quantity)"});
    for (auto& r : results) {
        write_csv_row(out, {
            csv_quote(r.c_name),
            std::to_string(r.c_custkey),
            std::to_string(r.o_orderkey),
            format_date(r.o_orderdate),
            fmt_money(r.o_totalprice, 2),
            fmt_money(r.sum_qty, 2)
        });
    }
}
