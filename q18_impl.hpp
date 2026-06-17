#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// q18: Large volume customer - orders where sum(l_quantity) > 314
// Join customer, orders, lineitem
// Group by c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice
// Order by o_totalprice DESC, o_orderdate

static inline std::string q18_days_to_date(int32_t days) {
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

static void run_q18(Database* db, const std::string& run_nr) {
    // First pass: compute sum(l_quantity) per orderkey
    std::unordered_map<int32_t, int64_t> order_qty_sum;
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        order_qty_sum[db->l_orderkey[i]] += db->l_quantity[i];
    }

    // Find orderkeys where sum > 314 (quantity is scale 2, so threshold = 31400)
    std::unordered_set<int32_t> big_orders;
    for (auto& [ok, qty] : order_qty_sum) {
        if (qty > 31400) {
            big_orders.insert(ok);
        }
    }

    // For qualifying orders, collect results
    struct Result {
        std::string c_name;
        int32_t c_custkey;
        int32_t o_orderkey;
        int32_t o_orderdate;
        int64_t o_totalprice;
        int64_t sum_qty;
    };
    std::vector<Result> results;

    for (int64_t i = 0; i < db->orders_count; i++) {
        int32_t ok = db->o_orderkey[i];
        if (big_orders.count(ok)) {
            int32_t ck = db->o_custkey[i];
            results.push_back({
                db->c_name[ck],
                ck,
                ok,
                db->o_orderdate[i],
                db->o_totalprice[i],
                order_qty_sum[ok]
            });
        }
    }

    // Sort by o_totalprice DESC, o_orderdate ASC
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        if (a.o_totalprice != b.o_totalprice) return a.o_totalprice > b.o_totalprice;
        return a.o_orderdate < b.o_orderdate;
    });

    std::ostringstream oss;
    write_csv_header(oss, {"c_name","c_custkey","o_orderkey","o_orderdate","o_totalprice","sum(l_quantity)"});
    for (auto& r : results) {
        write_csv_row(oss, {
            csv_quote(r.c_name),
            std::to_string(r.c_custkey),
            std::to_string(r.o_orderkey),
            q18_days_to_date(r.o_orderdate),
            fmt_decimal(r.o_totalprice / 100.0, 2),
            fmt_decimal(r.sum_qty / 100.0, 2)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
