#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
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
    TRACE_DECL(orders_scanned);
    {
        PROFILE_SCOPE("q22_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(orders_scanned);
            has_orders.insert(db->o_custkey[i]);
        }
    }
    TRACE_COUNT("q22_orders_scanned", orders_scanned);
    TRACE_COUNT("q22_build_rows", has_orders.size());

    // Compute avg(c_acctbal) for customers with acctbal > 0 and matching country code
    int64_t sum_for_avg = 0;
    int64_t count_for_avg = 0;
    int32_t max_custkey = (int32_t)db->c_phone.size() - 1;

    TRACE_DECL(cust_scanned_p1);
    {
        PROFILE_SCOPE("q22_avg_pass");
        for (int32_t ck = 1; ck <= max_custkey; ck++) {
            TRACE_INC(cust_scanned_p1);
            if (db->c_phone[ck].empty()) continue;
            std::string code = db->c_phone[ck].substr(0, 2);
            if (!valid_codes.count(code)) continue;
            if (db->c_acctbal[ck] > 0) {
                sum_for_avg += db->c_acctbal[ck];
                count_for_avg++;
            }
        }
    }
    TRACE_COUNT("q22_pass1_cust_scanned", cust_scanned_p1);

    double avg_acctbal = count_for_avg > 0 ? (double)sum_for_avg / count_for_avg : 0.0;

    // Find qualifying customers: matching code, acctbal > avg, no orders
    struct Agg { int64_t count = 0; int64_t total_acctbal = 0; };
    std::map<std::string, Agg> groups;

    TRACE_DECL(cust_scanned_p2);
    TRACE_DECL(cust_emitted);
    {
        PROFILE_SCOPE("q22_filter_probe_agg");
        for (int32_t ck = 1; ck <= max_custkey; ck++) {
            TRACE_INC(cust_scanned_p2);
            if (db->c_phone[ck].empty()) continue;
            std::string code = db->c_phone[ck].substr(0, 2);
            if (!valid_codes.count(code)) continue;
            if ((double)db->c_acctbal[ck] <= avg_acctbal) continue;
            if (has_orders.count(ck)) continue;
            TRACE_INC(cust_emitted);

            groups[code].count++;
            groups[code].total_acctbal += db->c_acctbal[ck];
        }
    }
    TRACE_COUNT("q22_pass2_cust_scanned", cust_scanned_p2);
    TRACE_COUNT("q22_cust_emitted", cust_emitted);
    TRACE_COUNT("q22_groups_created", groups.size());

    {
    PROFILE_SCOPE("q22_output");
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
    TRACE_COUNT("q22_query_output_rows", groups.size());
    }
    TRACE_FLUSH();
}
