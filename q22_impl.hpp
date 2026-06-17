#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>

inline void run_q22_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q22_total");
    TRACE_DECL_COUNTER(cust_scanned);
    TRACE_DECL_COUNTER(cust_emitted);

    // Target country codes (first 2 chars of phone) encoded as uint16 (c0<<8|c1).
    // Tiny query-time lookup table over the 2^16 possible 2-char prefixes.
    std::vector<uint8_t> is_target(65536, 0);
    {
        const char* codes[] = {"13","17","31","23","18","29","30"};
        for (const char* c : codes) {
            uint16_t k = (uint16_t)(((uint8_t)c[0] << 8) | (uint8_t)c[1]);
            is_target[k] = 1;
        }
    }

    // Anti-join against orders: cache-resident bitmap indexed by custkey marks
    // the existence of any order for that customer (RIGHT_ANTI semantics).
    // One bit per customer keeps the working set in L2 (~187KB at sf10).
    std::vector<uint64_t> has_orders((db->n_customer >> 6) + 2, 0);
    {
        PROFILE_SCOPE("q22_orders_scan");
        const int32_t no = db->n_orders;
        const int32_t* oc = db->o_custkey.data();
        uint64_t* bm = has_orders.data();
        for (int32_t i = 0; i < no; i++) {
            uint32_t k = (uint32_t)oc[i];
            bm[k >> 6] |= (uint64_t)1 << (k & 63);
        }
    }

    // Single customer pass (custkey == i+1, so the anti-join probe is a
    // sequential bit read): accumulate avg(c_acctbal) for the scalar subquery
    // over all prefix matches, and stash prefix-matched no-order customers.
    struct Cand { int64_t acctbal; uint16_t code; };
    std::vector<Cand> cands;
    cands.reserve((size_t)db->n_customer / 4 + 16);
    int64_t sum_bal = 0;
    int64_t count_bal = 0;
    {
        PROFILE_SCOPE("q22_customer_scan_filter_agg");
        const int32_t n = db->n_customer;
        const uint64_t* bm = has_orders.data();
        for (int32_t i = 0; i < n; i++) {
            TRACE_INC(cust_scanned);
            const std::string& ph = db->c_phone[i];
            uint16_t code = (uint16_t)(((uint8_t)ph[0] << 8) | (uint8_t)ph[1]);
            if (!is_target[code]) continue;
            int64_t bal = db->c_acctbal[i];
            if (bal > 0) {
                sum_bal += bal;
                count_bal++;
            }
            uint32_t k = (uint32_t)(i + 1);
            if ((bm[k >> 6] >> (k & 63)) & 1) continue;
            cands.push_back({bal, code});
        }
    }
    double avg_bal = (count_bal > 0) ? (double)sum_bal / count_bal : 0.0;

    struct Agg {
        int64_t numcust = 0;
        int64_t totacctbal = 0; // scale 2
    };
    std::map<std::string, Agg> groups;
    {
        PROFILE_SCOPE("q22_candidate_agg");
        for (const Cand& c : cands) {
            if ((double)c.acctbal <= avg_bal) continue;
            TRACE_INC(cust_emitted);
            char buf[2] = {(char)(c.code >> 8), (char)(c.code & 0xFF)};
            std::string code(buf, 2);
            Agg& a = groups[code];
            a.numcust++;
            a.totacctbal += c.acctbal;
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
