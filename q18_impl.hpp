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
    PROFILE_SCOPE("q18_total");
    TRACE_DECL_COUNTER(li_scanned);
    // First: find orderkeys where sum(l_quantity) > 314 (scale 2: 31400)
    const int64_t qty_threshold = 31400; // 314 * 100

    // Compute sum(l_quantity) per order using a dense array indexed by order
    // row index (via orderkey_to_idx). Avoids hashing the 60M-row scan.
    // Per-order quantity sum fits comfortably in int32 (max ~7 lineitems × 50 × 100).
    const int32_t n_orders = db->n_orders;
    std::vector<int32_t> order_qty_sum(n_orders, 0);
    const int32_t* __restrict ok_to_idx = db->orderkey_to_idx.data();
    {
        PROFILE_SCOPE("q18_lineitem_scan_agg");
        const int32_t* __restrict lok = db->l_orderkey.data();
        const int64_t* __restrict lqty = db->l_quantity.data();
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            int32_t o_idx = ok_to_idx[lok[i]];
            order_qty_sum[o_idx] += (int32_t)lqty[i];
        }
    }
    TRACE_COUNT("q18_rows_scanned", li_scanned);
    TRACE_COUNT("q18_agg_rows_in", li_scanned);
    TRACE_COUNT("q18_groups_created", (uint64_t)n_orders);

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

    {
        PROFILE_SCOPE("q18_order_join");
        const int32_t qty_thresh32 = (int32_t)qty_threshold;
        for (int32_t o_idx = 0; o_idx < n_orders; o_idx++) {
            int32_t sum = order_qty_sum[o_idx];
            if (sum <= qty_thresh32) continue;

            int32_t custkey = db->o_custkey[o_idx];
            int32_t c_idx = custkey - 1;

            results.push_back({
                db->c_name[c_idx],
                custkey,
                db->o_orderkey[o_idx],
                db->o_orderdate[o_idx],
                db->o_totalprice[o_idx],
                sum
            });
        }
    }
    TRACE_COUNT("q18_join_rows_emitted", (uint64_t)results.size());

    // Order by o_totalprice desc, o_orderdate
    TRACE_COUNT("q18_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q18_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.o_totalprice != b.o_totalprice) return a.o_totalprice > b.o_totalprice;
            return a.o_orderdate < b.o_orderdate;
        });
    }
    TRACE_COUNT("q18_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q18_output");
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
    TRACE_COUNT("q18_query_output_rows", (uint64_t)results.size());
}
