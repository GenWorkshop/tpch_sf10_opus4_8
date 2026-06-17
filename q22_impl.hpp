#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <unordered_set>

// q22: Global sales opportunity - customers with specific phone country codes,
// acctbal > avg(acctbal) for those codes where acctbal > 0, and no orders
// Group by cntrycode (first 2 chars of phone), count + sum acctbal
// Order by cntrycode

static void run_q22(Database* db, const std::string& run_nr) {
    // Valid country codes
    std::unordered_set<std::string> valid_codes = {"13","17","31","23","18","29","30"};

    // Build set of custkeys that have orders
    std::unordered_set<int32_t> has_orders;
    for (int64_t i = 0; i < db->orders_count; i++) {
        has_orders.insert(db->o_custkey[i]);
    }

    // Compute avg(c_acctbal) for customers with acctbal > 0 and matching country code
    int64_t sum_for_avg = 0;
    int64_t count_for_avg = 0;
    int32_t max_custkey = (int32_t)db->c_phone.size() - 1;

    for (int32_t ck = 1; ck <= max_custkey; ck++) {
        if (db->c_phone[ck].empty()) continue;
        std::string code = db->c_phone[ck].substr(0, 2);
        if (!valid_codes.count(code)) continue;
        if (db->c_acctbal[ck] > 0) {
            sum_for_avg += db->c_acctbal[ck];
            count_for_avg++;
        }
    }

    double avg_acctbal = count_for_avg > 0 ? (double)sum_for_avg / count_for_avg : 0.0;

    // Find qualifying customers: matching code, acctbal > avg, no orders
    struct Agg { int64_t count = 0; int64_t total_acctbal = 0; };
    std::map<std::string, Agg> groups;

    for (int32_t ck = 1; ck <= max_custkey; ck++) {
        if (db->c_phone[ck].empty()) continue;
        std::string code = db->c_phone[ck].substr(0, 2);
        if (!valid_codes.count(code)) continue;
        if ((double)db->c_acctbal[ck] <= avg_acctbal) continue;
        if (has_orders.count(ck)) continue;

        groups[code].count++;
        groups[code].total_acctbal += db->c_acctbal[ck];
    }

    std::ostringstream oss;
    write_csv_header(oss, {"cntrycode","numcust","totacctbal"});
    for (auto& [code, agg] : groups) {
        write_csv_row(oss, {
            code,
            std::to_string(agg.count),
            fmt_decimal(agg.total_acctbal / 100.0, 2)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
