#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>

// q13: Customer order count distribution
// LEFT OUTER JOIN customer with orders WHERE o_comment NOT LIKE '%express%requests%'
// Count orders per customer, then count customers per order-count
// Order by custdist DESC, c_count DESC

static void run_q13(Database* db, const std::string& run_nr) {
    // For each customer, count qualifying orders (o_comment NOT LIKE '%express%requests%')
    // Use custkey_to_order_idxs built in Database

    int32_t max_custkey = (int32_t)db->c_name.size() - 1;
    std::vector<int32_t> cust_order_count(max_custkey + 1, 0);

    for (int64_t i = 0; i < db->orders_count; i++) {
        const std::string& comment = db->o_comment[i];
        // NOT LIKE '%express%requests%'
        auto pos1 = comment.find("express");
        if (pos1 != std::string::npos) {
            auto pos2 = comment.find("requests", pos1 + 7);
            if (pos2 != std::string::npos) {
                continue; // exclude this order
            }
        }
        int32_t ck = db->o_custkey[i];
        cust_order_count[ck]++;
    }

    // Count distribution: c_count -> number of customers with that count
    std::unordered_map<int32_t, int64_t> dist;
    for (int32_t ck = 1; ck <= max_custkey; ck++) {
        // Only count valid customers (those that exist in the data)
        if (!db->c_name[ck].empty()) {
            dist[cust_order_count[ck]]++;
        }
    }

    // Collect and sort by custdist DESC, c_count DESC
    struct Result { int32_t c_count; int64_t custdist; };
    std::vector<Result> results;
    for (auto& [cnt, cdist] : dist) {
        results.push_back({cnt, cdist});
    }
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        if (a.custdist != b.custdist) return a.custdist > b.custdist;
        return a.c_count > b.c_count;
    });

    std::ostringstream oss;
    write_csv_header(oss, {"c_count","custdist"});
    for (auto& r : results) {
        write_csv_row(oss, {
            std::to_string(r.c_count),
            std::to_string(r.custdist)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
