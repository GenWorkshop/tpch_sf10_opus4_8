#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

inline void run_q17_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q17_total");
    TRACE_DECL_COUNTER(scan1);
    TRACE_DECL_COUNTER(scan1_emit);
    TRACE_DECL_COUNTER(scan2);
    TRACE_DECL_COUNTER(scan2_emit);
    // Find parts: p_brand = 'Brand#13' AND p_container = 'MED BOX'
    std::unordered_set<int32_t> matching_partkeys; // 1-based
    for (int32_t i = 0; i < db->n_part; i++) {
        if (db->p_brand[i] == "Brand#13" && db->p_container[i] == "MED BOX") {
            matching_partkeys.insert(i + 1);
        }
    }

    // For each matching part, compute avg(l_quantity) from all lineitems with that partkey
    // Then sum extendedprice where l_quantity < 0.2 * avg
    // First pass: compute sum and count of quantity per partkey
    struct PartStats {
        int64_t sum_qty = 0; // scale 2
        int64_t count = 0;
    };
    std::unordered_map<int32_t, PartStats> part_stats;

    {
        PROFILE_SCOPE("q17_lineitem_scan_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(scan1);
            int32_t pk = db->l_partkey[i];
            if (matching_partkeys.count(pk)) {
                TRACE_INC(scan1_emit);
                part_stats[pk].sum_qty += db->l_quantity[i];
                part_stats[pk].count++;
            }
        }
    }
    TRACE_COUNT("q17_rows_scanned", scan1);
    TRACE_COUNT("q17_rows_emitted", scan1_emit);
    TRACE_COUNT("q17_groups_created", (uint64_t)part_stats.size());

    // Second pass: sum extendedprice where quantity < 0.2 * avg(quantity)
    // avg_qty = sum_qty / count (both in scale 2, so avg is scale 2)
    // threshold = 0.2 * avg_qty = 0.2 * sum_qty / count
    // l_quantity < threshold → l_quantity * count * 5 < sum_qty (multiply both sides by count*5)
    double total_price = 0.0;

    {
        PROFILE_SCOPE("q17_lineitem_scan_filter");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(scan2);
            int32_t pk = db->l_partkey[i];
            if (!matching_partkeys.count(pk)) continue;
            auto& stats = part_stats[pk];
            if (stats.count == 0) continue;

            // Check: l_quantity < 0.2 * (sum_qty / count)
            // Equivalent: l_quantity * count * 5 < sum_qty (avoiding floating point)
            if ((int64_t)db->l_quantity[i] * stats.count * 5 < stats.sum_qty) {
                TRACE_INC(scan2_emit);
                total_price += db->l_extendedprice[i]; // scale 2
            }
        }
    }
    TRACE_COUNT("q17_rows_scanned_pass2", scan2);
    TRACE_COUNT("q17_rows_emitted_pass2", scan2_emit);
    TRACE_COUNT("q17_agg_rows_in", scan2_emit);

    // Result: sum(l_extendedprice) / 7.0
    // total_price is in scale 2 (cents), divide by 100 to get dollars, then /7
    double avg_yearly = (total_price / 100.0) / 7.0;

    PROFILE_SCOPE("q17_output");
    write_csv_header(out, {"avg_yearly"});
    write_csv_row(out, {fmt_decimal(avg_yearly, 15)});
    TRACE_COUNT("q17_query_output_rows", 1);
}
