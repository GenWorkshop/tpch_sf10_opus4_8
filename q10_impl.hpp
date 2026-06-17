#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// q10: customer, orders, lineitem, nation
// Filter: o_orderdate in [1993-08-01, 1993-11-01), l_returnflag = 'R'
// Join: c_custkey=o_custkey, l_orderkey=o_orderkey, c_nationkey=n_nationkey
// Group by custkey (and customer fields), sum revenue = price*(1-disc) scale4
// Order by revenue DESC

static void run_q10(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1993, 8, 1);
    const int32_t date_end = to_days(1993, 11, 1);

    // Qualifying orders: orderdate in range -> map orderkey -> custkey
    std::unordered_map<int32_t, int32_t> order_to_cust;
    for (int64_t i = 0; i < db->orders_count; i++) {
        int32_t od = db->o_orderdate[i];
        if (od >= date_start && od < date_end) {
            order_to_cust[db->o_orderkey[i]] = db->o_custkey[i];
        }
    }

    // Scan lineitem: returnflag='R', orderkey in qualifying orders
    // Group by custkey -> revenue
    std::unordered_map<int32_t, int64_t> cust_revenue;
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        if (db->l_returnflag[i] != 'R') continue;
        int32_t ok = db->l_orderkey[i];
        auto it = order_to_cust.find(ok);
        if (it == order_to_cust.end()) continue;
        int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]);
        cust_revenue[it->second] += rev;
    }

    // Collect results
    struct Result {
        int32_t custkey;
        int64_t revenue;
    };
    std::vector<Result> results;
    results.reserve(cust_revenue.size());
    for (auto& [ck, rev] : cust_revenue) {
        results.push_back({ck, rev});
    }

    // Sort by revenue DESC
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        return a.revenue > b.revenue;
    });

    std::ostringstream oss;
    write_csv_header(oss, {"c_custkey","c_name","revenue","c_acctbal","n_name","c_address","c_phone","c_comment"});
    for (auto& r : results) {
        int32_t ck = r.custkey;
        write_csv_row(oss, {
            std::to_string(ck),
            csv_quote(db->c_name[ck]),
            fmt_decimal(r.revenue / 10000.0, 4),
            fmt_decimal(db->c_acctbal[ck] / 100.0, 2),
            csv_quote(db->nationkey_to_name[db->c_nationkey[ck]]),
            csv_quote(db->c_address[ck]),
            csv_quote(db->c_phone[ck]),
            csv_quote(db->c_comment[ck])
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
