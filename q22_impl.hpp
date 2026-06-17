#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_set>
#include <map>
#include <string>

inline void run_q22_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q22_total");
    TRACE_DECL_COUNTER(cust_scanned);
    TRACE_DECL_COUNTER(cust_emitted);
    // Target country codes (first 2 chars of phone)
    std::unordered_set<std::string> target_codes = {"13","17","31","23","18","29","30"};

    // Build set of custkeys that have orders
    std::unordered_set<int32_t> has_orders;
    {
        PROFILE_SCOPE("q22_orders_scan");
        for (int32_t i = 0; i < db->n_orders; i++) {
            has_orders.insert(db->o_custkey[i]);
        }
    }

    // Compute avg(c_acctbal) for customers with acctbal > 0 and phone prefix in target
    int64_t sum_bal = 0;
    int64_t count_bal = 0;
    {
        PROFILE_SCOPE("q22_customer_avg_scan");
        for (int32_t i = 0; i < db->n_customer; i++) {
            std::string prefix = db->c_phone[i].substr(0, 2);
            if (target_codes.count(prefix) && db->c_acctbal[i] > 0) {
                sum_bal += db->c_acctbal[i];
                count_bal++;
            }
        }
    }
    // avg in scale 2 (cents)
    double avg_bal = (count_bal > 0) ? (double)sum_bal / count_bal : 0.0;

    // Find qualifying customers: phone prefix in target, acctbal > avg, no orders
    struct Agg {
        int64_t numcust = 0;
        int64_t totacctbal = 0; // scale 2
    };
    std::map<std::string, Agg> groups;

    {
        PROFILE_SCOPE("q22_customer_scan_filter_agg");
        for (int32_t i = 0; i < db->n_customer; i++) {
            TRACE_INC(cust_scanned);
            std::string prefix = db->c_phone[i].substr(0, 2);
            if (!target_codes.count(prefix)) continue;
            if ((double)db->c_acctbal[i] <= avg_bal) continue;
            int32_t custkey = i + 1;
            if (has_orders.count(custkey)) continue;

            TRACE_INC(cust_emitted);
            groups[prefix].numcust++;
            groups[prefix].totacctbal += db->c_acctbal[i];
        }
    }
    TRACE_COUNT("q22_rows_scanned", cust_scanned);
    TRACE_COUNT("q22_rows_emitted", cust_emitted);
    TRACE_COUNT("q22_agg_rows_in", cust_emitted);
    TRACE_COUNT("q22_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q22_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q22_output");
    write_csv_header(out, {"cntrycode","numcust","totacctbal"});
    for (auto& [code, g] : groups) {
        write_csv_row(out, {
            code,
            std::to_string(g.numcust),
            fmt_money(g.totacctbal, 2)
        });
    }
    TRACE_COUNT("q22_query_output_rows", (uint64_t)groups.size());
}
