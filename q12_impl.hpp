#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <map>
#include <memory>

inline void run_q12_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q12_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // l_receiptdate >= '1997-01-01' AND l_receiptdate < '1998-01-01'
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    // Group by l_shipmode
    struct Agg {
        int64_t high_count = 0;
        int64_t low_count = 0;
    };

    // Two fixed groups (MAIL, FOB) avoid std::map / string-keyed lookups in the
    // hot loop.  Cheap integer date predicates run first so the std::string
    // shipmode column is only touched for the small surviving fraction of rows.
    Agg agg_mail;
    Agg agg_fob;
    const Date* __restrict commitdate  = db->l_commitdate.data();
    const Date* __restrict receiptdate = db->l_receiptdate.data();
    const Date* __restrict shipdate    = db->l_shipdate.data();
    const int32_t* __restrict orderkey = db->l_orderkey.data();
    const std::string* __restrict shipmode = db->l_shipmode.data();

    {
        PROFILE_SCOPE("q12_lineitem_scan_join_agg");
        const int64_t n = db->n_lineitem;
        // Pass 1: branchless collection of rows whose l_receiptdate falls in the
        // target year.  Writing the index unconditionally and bumping the count
        // by the (0/1) predicate eliminates the hard-to-predict range branch that
        // otherwise dominates the 60M-row scan.
        std::unique_ptr<int32_t[]> survivors(new int32_t[n]);
        int64_t count = 0;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            const Date rd = receiptdate[i];
            survivors[count] = (int32_t)i;
            count += (rd >= date_lo) & (rd < date_hi);
        }

        // Pass 2: only the ~1/7 receiptdate survivors pay for the remaining
        // (cache-unfriendly) date, shipmode and orders probes.  Survivor indices
        // are ascending but sparse, so software-prefetch the scattered columns a
        // fixed distance ahead to hide the cache-miss latency.
        constexpr int64_t PF = 48;
        for (int64_t s = 0; s < count; s++) {
            if (s + PF < count) {
                const int32_t j = survivors[s + PF];
                __builtin_prefetch(&commitdate[j], 0, 1);
                __builtin_prefetch(&shipdate[j], 0, 1);
                __builtin_prefetch(&shipmode[j], 0, 1);
                __builtin_prefetch(&orderkey[j], 0, 1);
            }
            const int32_t i = survivors[s];
            const Date rd = receiptdate[i];
            const Date cd = commitdate[i];
            if (cd >= rd) continue;
            if (shipdate[i] >= cd) continue;

            // l_shipmode in ('MAIL', 'FOB')
            const std::string& mode = shipmode[i];
            Agg* g;
            if (mode == "MAIL") {
                g = &agg_mail;
            } else if (mode == "FOB") {
                g = &agg_fob;
            } else {
                continue;
            }

            // Get order priority (perfect-hash probe into orders)
            int32_t ok = orderkey[i];
            if (ok > db->max_orderkey) continue;
            int32_t o_idx = db->orderkey_to_idx[ok];
            if (o_idx < 0) continue;

            TRACE_INC(li_emitted);
            const std::string& prio = db->o_orderpriority[o_idx];
            if (prio == "1-URGENT" || prio == "2-HIGH") {
                g->high_count++;
            } else {
                g->low_count++;
            }
        }
    }

    std::map<std::string, Agg> groups;
    groups["FOB"] = agg_fob;
    groups["MAIL"] = agg_mail;
    TRACE_COUNT("q12_rows_scanned", li_scanned);
    TRACE_COUNT("q12_join_rows_emitted", li_emitted);
    TRACE_COUNT("q12_agg_rows_in", li_emitted);
    TRACE_COUNT("q12_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q12_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q12_output");
    write_csv_header(out, {"l_shipmode","high_line_count","low_line_count"});
    for (auto& [mode, g] : groups) {
        write_csv_row(out, {
            mode,
            fmt_decimal(static_cast<double>(g.high_count), 1),
            fmt_decimal(static_cast<double>(g.low_count), 1)
        });
    }
    TRACE_COUNT("q12_query_output_rows", (uint64_t)groups.size());
}
