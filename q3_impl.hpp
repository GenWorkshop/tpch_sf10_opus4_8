#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // for date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

// Format epoch days as YYYY-MM-DD
inline std::string format_date(int32_t days) {
    // Civil date from days since 1970-01-01 (Howard Hinnant algorithm)
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned d = doy - (153*mp + 2)/5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return std::string(buf);
}

inline void run_q3_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q3_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Filters:
    // c_mktsegment = 'BUILDING'
    // o_orderdate < '1995-03-08'
    // l_shipdate > '1995-03-08'
    const Date date_filter = date_to_epoch(1995, 3, 8);

    // Step 1: Find customers in BUILDING segment
    std::vector<bool> cust_building(db->n_customer, false);
    for (int32_t i = 0; i < db->n_customer; i++) {
        if (db->c_mktsegment[i] == "BUILDING") {
            cust_building[i] = true;
        }
    }

    // Step 2: Find qualifying orders (customer in BUILDING, orderdate < 1995-03-08)
    // Store order index → true
    std::vector<bool> order_qualifies(db->n_orders, false);
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderdate[i] < date_filter) {
            int32_t custkey = db->o_custkey[i];
            int32_t c_idx = custkey - 1;
            if (c_idx >= 0 && c_idx < db->n_customer && cust_building[c_idx]) {
                order_qualifies[i] = true;
            }
        }
    }

    // Step 3: Scan lineitem, filter l_shipdate > date_filter, join with qualifying orders
    // Group by (l_orderkey, o_orderdate, o_shippriority) → sum revenue
    struct GroupVal {
        int64_t revenue; // scale 4: extprice(s2) * (100 - disc(s2)) = scale 4
        Date o_orderdate;
        int32_t o_shippriority;
    };
    std::unordered_map<int32_t, GroupVal> groups; // key = orderkey

    {
        PROFILE_SCOPE("q3_lineitem_scan_join_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            if (db->l_shipdate[i] > date_filter) {
                int32_t orderkey = db->l_orderkey[i];
                if (orderkey <= db->max_orderkey) {
                    int32_t o_idx = db->orderkey_to_idx[orderkey];
                    if (o_idx >= 0 && order_qualifies[o_idx]) {
                        TRACE_INC(li_emitted);
                        int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]);
                        auto it = groups.find(orderkey);
                        if (it == groups.end()) {
                            groups[orderkey] = {rev, db->o_orderdate[o_idx], db->o_shippriority[o_idx]};
                        } else {
                            it->second.revenue += rev;
                        }
                    }
                }
            }
        }
    }
    TRACE_COUNT("q3_rows_scanned", li_scanned);
    TRACE_COUNT("q3_join_rows_emitted", li_emitted);
    TRACE_COUNT("q3_agg_rows_in", li_emitted);
    TRACE_COUNT("q3_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q3_agg_rows_emitted", (uint64_t)groups.size());

    // Step 4: Sort by revenue desc, o_orderdate asc
    struct ResultRow {
        int32_t orderkey;
        int64_t revenue;
        Date o_orderdate;
        int32_t o_shippriority;
    };
    std::vector<ResultRow> results;
    results.reserve(groups.size());
    for (auto& [ok, g] : groups) {
        results.push_back({ok, g.revenue, g.o_orderdate, g.o_shippriority});
    }
    TRACE_COUNT("q3_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q3_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.revenue != b.revenue) return a.revenue > b.revenue;
            return a.o_orderdate < b.o_orderdate;
        });
    }
    TRACE_COUNT("q3_sort_rows_out", (uint64_t)results.size());

    // Output
    PROFILE_SCOPE("q3_output");
    write_csv_header(out, {"l_orderkey","revenue","o_orderdate","o_shippriority"});
    for (auto& r : results) {
        write_csv_row(out, {
            std::to_string(r.orderkey),
            fmt_money(r.revenue, 4),
            format_date(r.o_orderdate),
            std::to_string(r.o_shippriority)
        });
    }
    TRACE_COUNT("q3_query_output_rows", (uint64_t)results.size());
}
