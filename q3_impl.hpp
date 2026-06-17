#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// q3: customer(BUILDING) join orders(orderdate < 1995-03-08) join lineitem(shipdate > 1995-03-08)
// group by l_orderkey, o_orderdate, o_shippriority
// revenue = sum(l_extendedprice * (1 - l_discount))  -> scale 4
// order by revenue desc, o_orderdate

static inline int32_t q3_date_to_days(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

static inline std::string days_to_date_str(int32_t days) {
    // Convert days since 1970-01-01 to YYYY-MM-DD
    days += 719468;
    int era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(days - era * 146097);
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

static void run_q3(Database* db, const std::string& run_nr) {
    const int32_t date_1995_03_08 = q3_date_to_days(1995, 3, 8);

    // Build set of custkeys with mktsegment = 'BUILDING'
    std::unordered_set<int32_t> building_custkeys;
    {
        PROFILE_SCOPE("q3_cust_build");
        for (int32_t ck = 1; ck < (int32_t)db->c_mktsegment.size(); ck++) {
            if (db->c_mktsegment[ck] == "BUILDING") {
                building_custkeys.insert(ck);
            }
        }
    }
    TRACE_COUNT("q3_building_custkeys", building_custkeys.size());

    // Find qualifying orders: orderdate < 1995-03-08 and custkey in BUILDING
    // Map orderkey -> (orderdate, shippriority)
    struct OrderInfo { int32_t orderdate; int32_t shippriority; };
    std::unordered_map<int32_t, OrderInfo> qualifying_orders;
    TRACE_DECL(orders_scanned);
    {
        PROFILE_SCOPE("q3_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(orders_scanned);
            if (db->o_orderdate[i] < date_1995_03_08 &&
                building_custkeys.count(db->o_custkey[i])) {
                qualifying_orders[db->o_orderkey[i]] = {db->o_orderdate[i], db->o_shippriority[i]};
            }
        }
    }
    TRACE_COUNT("q3_orders_scanned", orders_scanned);
    TRACE_COUNT("q3_build_rows", qualifying_orders.size());

    // Scan lineitem: shipdate > 1995-03-08, orderkey in qualifying_orders
    // Group by (orderkey, orderdate, shippriority) - but orderkey determines the other two
    struct GroupAgg {
        int64_t revenue = 0; // scale 4: price * (100 - discount)
        int32_t orderdate;
        int32_t shippriority;
    };
    std::unordered_map<int32_t, GroupAgg> groups;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(probe_rows_in);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q3_lineitem_probe_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            if (db->l_shipdate[i] > date_1995_03_08) {
                TRACE_INC(probe_rows_in);
                int32_t ok = db->l_orderkey[i];
                auto it = qualifying_orders.find(ok);
                if (it != qualifying_orders.end()) {
                    TRACE_INC(join_rows_emitted);
                    auto& g = groups[ok];
                    if (g.revenue == 0 && g.orderdate == 0) {
                        g.orderdate = it->second.orderdate;
                        g.shippriority = it->second.shippriority;
                    }
                    // revenue = extendedprice * (1 - discount)
                    // price is scale2, discount is scale2, product is scale4
                    int64_t disc_price = db->l_extendedprice[i] * (100 - db->l_discount[i]);
                    g.revenue += disc_price;
                }
            }
        }
    }
    TRACE_COUNT("q3_rows_scanned", rows_scanned);
    TRACE_COUNT("q3_probe_rows_in", probe_rows_in);
    TRACE_COUNT("q3_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q3_groups_created", groups.size());

    // Collect results
    struct Result {
        int32_t orderkey;
        int64_t revenue;
        int32_t orderdate;
        int32_t shippriority;
    };
    std::vector<Result> results;
    results.reserve(groups.size());
    for (auto& [ok, g] : groups) {
        results.push_back({ok, g.revenue, g.orderdate, g.shippriority});
    }

    {
        PROFILE_SCOPE("q3_sort");
        // Sort by revenue desc, orderdate asc
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            if (a.revenue != b.revenue) return a.revenue > b.revenue;
            return a.orderdate < b.orderdate;
        });
    }
    TRACE_COUNT("q3_sort_rows_in", results.size());
    TRACE_COUNT("q3_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q3_output");
    std::ostringstream oss;
    write_csv_header(oss, {"l_orderkey","revenue","o_orderdate","o_shippriority"});

    for (auto& r : results) {
        write_csv_row(oss, {
            std::to_string(r.orderkey),
            fmt_decimal(r.revenue / 10000.0, 4),
            days_to_date_str(r.orderdate),
            std::to_string(r.shippriority)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q3_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
