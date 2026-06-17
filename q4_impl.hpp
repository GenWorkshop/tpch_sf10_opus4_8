#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q3_impl.hpp" // format_date
#include <ostream>
#include <vector>
#include <map>
#include <unordered_set>

inline void run_q4_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q4_total");
    TRACE_DECL_COUNTER(orders_scanned);
    TRACE_DECL_COUNTER(orders_emitted);
    TRACE_DECL_COUNTER(li_probed);
    // o_orderdate >= '1993-09-01' AND o_orderdate < '1993-12-01'
    const Date date_lo = date_to_epoch(1993, 9, 1);
    const Date date_hi = date_to_epoch(1993, 12, 1);

    // o_orderpriority is one of 5 values "1-URGENT".."5-LOW"; bucket by first
    // digit (1..9 -> idx 0..8) and remember the full string for output.
    int64_t cnt[10] = {0};
    const std::string* repr[10] = {nullptr};

    if (db->lineitem_sorted_by_orderkey) {
        const Date* __restrict odate = db->o_orderdate.data();
        const int32_t* __restrict okey = db->o_orderkey.data();
        const Database::LineitemRange* __restrict lrng = db->orderkey_lineitem_range.data();
        const Date* __restrict lc = db->l_commitdate.data();
        const Date* __restrict lr = db->l_receiptdate.data();

        // Phase 1: tight sequential scan of the date column, collecting the
        // (sparse, ~3.8%) order rows that pass the date filter. Branchless
        // append avoids misprediction on the selective predicate.
        std::vector<int32_t> pass(db->n_orders);
        int32_t* __restrict pbuf = pass.data();
        size_t npass = 0;
        {
            PROFILE_SCOPE("q4_orders_date_filter");
            for (int32_t i = 0; i < db->n_orders; i++) {
                TRACE_INC(orders_scanned);
                Date d = odate[i];
                pbuf[npass] = i;
                npass += (size_t)(d >= date_lo && d < date_hi);
            }
        }

        // Phase 2: probe lineitem CSR for each passing order with a software
        // prefetch pipeline. The CSR boundary arrays (ls/le) and lineitem date
        // columns are large and accessed at data-dependent offsets, so we issue
        // prefetches well ahead of use to hide DRAM latency.
        {
            PROFILE_SCOPE("q4_orders_scan_agg");
            const int n = (int)npass;
            const int PF1 = 64;  // prefetch CSR boundaries this far ahead
            const int PF2 = 32;  // prefetch lineitem dates this far ahead
            for (int k = 0; k < n; k++) {
                if (k + PF1 < n) {
                    int32_t fok = okey[pbuf[k + PF1]];
                    __builtin_prefetch(&lrng[fok], 0, 1);
                }
                if (k + PF2 < n) {
                    int32_t mok = okey[pbuf[k + PF2]];
                    int32_t ms = lrng[mok].start;
                    __builtin_prefetch(&lc[ms], 0, 1);
                    __builtin_prefetch(&lr[ms], 0, 1);
                }
                int32_t row = pbuf[k];
                int32_t ok = okey[row];
                int32_t start = lrng[ok].start;
                int32_t end = lrng[ok].end;
                bool late = false;
                for (int32_t j = start; j < end; j++) {
                    TRACE_INC(li_probed);
                    if (lc[j] < lr[j]) { late = true; break; }
                }
                if (late) {
                    TRACE_INC(orders_emitted);
                    const std::string& p = db->o_orderpriority[row];
                    int b = (int)((unsigned char)p[0] - '1');
                    cnt[b]++;
                    if (!repr[b]) repr[b] = &p;
                }
            }
        }
    } else {
        // Fallback: build late-order bitmap from full lineitem scan, then aggregate.
        std::vector<bool> order_has_late(db->max_orderkey + 1, false);
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_probed);
            if (db->l_commitdate[i] < db->l_receiptdate[i]) {
                int32_t ok = db->l_orderkey[i];
                if (ok <= db->max_orderkey) order_has_late[ok] = true;
            }
        }
        for (int32_t i = 0; i < db->n_orders; i++) {
            TRACE_INC(orders_scanned);
            if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
                if (order_has_late[db->o_orderkey[i]]) {
                    TRACE_INC(orders_emitted);
                    const std::string& p = db->o_orderpriority[i];
                    int b = (int)((unsigned char)p[0] - '1');
                    cnt[b]++;
                    if (!repr[b]) repr[b] = &p;
                }
            }
        }
    }

    TRACE_COUNT("q4_probe_rows_in", li_probed);
    TRACE_COUNT("q4_rows_scanned", orders_scanned);
    TRACE_COUNT("q4_rows_emitted", orders_emitted);

    PROFILE_SCOPE("q4_output");
    write_csv_header(out, {"o_orderpriority","order_count"});
    for (int b = 0; b < 10; b++) {
        if (repr[b]) write_csv_row(out, {*repr[b], std::to_string(cnt[b])});
    }
}
