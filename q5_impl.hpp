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

// q5: customer, orders, lineitem, supplier, nation, region
// Join: c_custkey=o_custkey, l_orderkey=o_orderkey, l_suppkey=s_suppkey,
//       c_nationkey=s_nationkey, s_nationkey=n_nationkey, n_regionkey=r_regionkey
// Filter: r_name='AFRICA', o_orderdate in [1997-01-01, 1998-01-01)
// Group by n_name, sum revenue = l_extendedprice*(1-l_discount) (scale 4)
// Order by revenue desc

static void run_q5(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1997, 1, 1);
    const int32_t date_end = to_days(1998, 1, 1);

    // Find AFRICA region key
    int32_t africa_rk = db->region_name_to_key["AFRICA"];

    // Nations in AFRICA
    std::unordered_set<int32_t> africa_nations;
    for (int32_t nk : db->region_to_nation_keys[africa_rk]) {
        africa_nations.insert(nk);
    }

    // Qualifying orders: orderdate in range, custkey's nation is in AFRICA
    // Map orderkey -> custkey's nationkey
    std::unordered_map<int32_t, int32_t> order_to_nation; // orderkey -> customer nationkey
    TRACE_DECL(orders_scanned);
    {
        PROFILE_SCOPE("q5_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(orders_scanned);
            int32_t od = db->o_orderdate[i];
            if (od >= date_start && od < date_end) {
                int32_t ck = db->o_custkey[i];
                int32_t cnk = db->c_nationkey[ck];
                if (africa_nations.count(cnk)) {
                    order_to_nation[db->o_orderkey[i]] = cnk;
                }
            }
        }
    }
    TRACE_COUNT("q5_orders_scanned", orders_scanned);
    TRACE_COUNT("q5_build_rows", order_to_nation.size());

    // Scan lineitem: join with qualifying orders, require supplier in same nation as customer
    // revenue per nation
    std::unordered_map<int32_t, int64_t> nation_revenue; // nationkey -> revenue (scale 4)

    TRACE_DECL(rows_scanned);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q5_lineitem_probe_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t ok = db->l_orderkey[i];
            auto it = order_to_nation.find(ok);
            if (it != order_to_nation.end()) {
                int32_t cust_nk = it->second;
                int32_t sk = db->l_suppkey[i];
                // c_nationkey = s_nationkey condition
                if (db->s_nationkey[sk] == cust_nk) {
                    TRACE_INC(join_rows_emitted);
                    int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]);
                    nation_revenue[cust_nk] += rev;
                }
            }
        }
    }
    TRACE_COUNT("q5_rows_scanned", rows_scanned);
    TRACE_COUNT("q5_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q5_groups_created", nation_revenue.size());

    // Collect and sort by revenue desc
    struct Result { std::string name; int64_t revenue; };
    std::vector<Result> results;
    for (auto& [nk, rev] : nation_revenue) {
        results.push_back({db->nationkey_to_name[nk], rev});
    }
    {
        PROFILE_SCOPE("q5_sort");
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q5_sort_rows_in", results.size());
    TRACE_COUNT("q5_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q5_output");
    std::ostringstream oss;
    write_csv_header(oss, {"n_name","revenue"});
    for (auto& r : results) {
        write_csv_row(oss, {
            csv_quote(r.name),
            fmt_decimal(r.revenue / 10000.0, 4)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q5_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
