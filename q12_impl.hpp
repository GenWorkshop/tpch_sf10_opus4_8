#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <map>

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
    std::map<std::string, Agg> groups;

    {
        PROFILE_SCOPE("q12_lineitem_scan_join_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            // l_shipmode in ('MAIL', 'FOB')
            const auto& mode = db->l_shipmode[i];
            if (mode != "MAIL" && mode != "FOB") continue;

            // l_commitdate < l_receiptdate
            if (db->l_commitdate[i] >= db->l_receiptdate[i]) continue;

            // l_shipdate < l_commitdate
            if (db->l_shipdate[i] >= db->l_commitdate[i]) continue;

            // l_receiptdate >= date_lo AND < date_hi
            if (db->l_receiptdate[i] < date_lo || db->l_receiptdate[i] >= date_hi) continue;

            // Get order priority
            int32_t orderkey = db->l_orderkey[i];
            if (orderkey > db->max_orderkey) continue;
            int32_t o_idx = db->orderkey_to_idx[orderkey];
            if (o_idx < 0) continue;

            TRACE_INC(li_emitted);
            const auto& prio = db->o_orderpriority[o_idx];
            auto& g = groups[mode];
            if (prio == "1-URGENT" || prio == "2-HIGH") {
                g.high_count++;
            } else {
                g.low_count++;
            }
        }
    }
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
