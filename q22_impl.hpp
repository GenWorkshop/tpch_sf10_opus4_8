#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <algorithm>

inline void run_q22_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q22_total");
    TRACE_DECL_COUNTER(cust_scanned);
    TRACE_DECL_COUNTER(cust_emitted);

    // Target country codes (first 2 chars of phone) encoded as uint16 (c0<<8|c1).
    // Tiny query-time lookup table over the 2^16 possible 2-char prefixes.
    std::vector<uint8_t> is_target(65536, 0);
    // Map each target code to a dense slot index for branch-light grouping.
    std::vector<int8_t> code_slot(65536, -1);
    {
        const char* codes[] = {"13","17","31","23","18","29","30"};
        int8_t s = 0;
        for (const char* c : codes) {
            uint16_t k = (uint16_t)(((uint8_t)c[0] << 8) | (uint8_t)c[1]);
            is_target[k] = 1;
            code_slot[k] = s++;
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
    cands.reserve((size_t)db->n_customer + 1);
    int64_t sum_bal = 0;
    int64_t count_bal = 0;
    size_t ncand = 0;
    {
        PROFILE_SCOPE("q22_customer_scan_filter_agg");
        const int32_t n = db->n_customer;
        const uint64_t* __restrict bm = has_orders.data();
        const std::string* __restrict phone = db->c_phone.data();
        const int64_t* __restrict acct = db->c_acctbal.data();
        const uint8_t* __restrict tgt = is_target.data();
        Cand* __restrict cbuf = cands.data();
        for (int32_t i = 0; i < n; i++) {
            TRACE_INC(cust_scanned);
            const char* p = phone[i].data();
            uint16_t code = (uint16_t)(((uint8_t)p[0] << 8) | (uint8_t)p[1]);
            int64_t bal = acct[i];
            uint32_t k = (uint32_t)(i + 1);
            int64_t hit = tgt[code];
            int64_t pos = hit & (bal > 0);
            sum_bal += bal & -pos;
            count_bal += pos;
            int64_t hasord = (bm[k >> 6] >> (k & 63)) & 1;
            int64_t keep = hit & (hasord ^ 1);
            cbuf[ncand] = {bal, code};
            ncand += (size_t)keep;
        }
    }
    double avg_bal = (count_bal > 0) ? (double)sum_bal / count_bal : 0.0;

    struct Agg {
        int64_t numcust = 0;
        int64_t totacctbal = 0; // scale 2
        uint16_t code = 0;
    };
    Agg slots[7];
    {
        PROFILE_SCOPE("q22_candidate_agg");
        const Cand* __restrict cbuf = cands.data();
        for (size_t ci = 0; ci < ncand; ci++) {
            Cand c = cbuf[ci];
            if ((double)c.acctbal <= avg_bal) continue;
            TRACE_INC(cust_emitted);
            int8_t s = code_slot[c.code];
            slots[s].numcust++;
            slots[s].totacctbal += c.acctbal;
            slots[s].code = c.code;
        }
    }
    std::vector<Agg> groups;
    for (int s = 0; s < 7; s++) {
        if (slots[s].numcust > 0) groups.push_back(slots[s]);
    }
    std::sort(groups.begin(), groups.end(),
              [](const Agg& a, const Agg& b) { return a.code < b.code; });
    TRACE_COUNT("q22_rows_scanned", cust_scanned);
    TRACE_COUNT("q22_rows_emitted", cust_emitted);
    TRACE_COUNT("q22_agg_rows_in", cust_emitted);
    TRACE_COUNT("q22_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q22_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q22_output");
    write_csv_header(out, {"cntrycode","numcust","totacctbal"});
    for (const Agg& g : groups) {
        char buf[2] = {(char)(g.code >> 8), (char)(g.code & 0xFF)};
        write_csv_row(out, {
            std::string(buf, 2),
            std::to_string(g.numcust),
            fmt_money(g.totacctbal, 2)
        });
    }
    TRACE_COUNT("q22_query_output_rows", (uint64_t)groups.size());
}
