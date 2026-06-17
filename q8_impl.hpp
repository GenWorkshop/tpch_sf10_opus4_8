#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q7_impl.hpp" // epoch_to_year
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <algorithm>

inline void run_q8_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q8_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Filter: p_type = 'ECONOMY BRUSHED TIN'
    std::unordered_set<int32_t> matching_partkeys; // 1-based
    for (int32_t i = 0; i < db->n_part; i++) {
        if (db->p_type[i] == "ECONOMY BRUSHED TIN") {
            matching_partkeys.insert(i + 1);
        }
    }

    // Filter: r_name = 'AMERICA' → customer nations in AMERICA
    int32_t america_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "AMERICA") { america_regionkey = i; break; }
    }
    std::unordered_set<int32_t> america_nations;
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == america_regionkey) america_nations.insert(i);
    }

    // Nation for FRANCE
    int32_t france_nk = db->nation_name_to_key["FRANCE"];

    // Date filter: o_orderdate between '1995-01-01' and '1996-12-31'
    const Date date_lo = date_to_epoch(1995, 1, 1);
    const Date date_hi = date_to_epoch(1996, 12, 31);

    // Qualify orders: orderdate in range AND customer in AMERICA region
    // Store order_idx → year (or -1 if not qualifying)
    std::vector<int16_t> order_year(db->n_orders, -1);
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] <= date_hi) {
            int32_t custkey = db->o_custkey[i];
            int32_t c_idx = custkey - 1;
            if (c_idx >= 0 && c_idx < db->n_customer) {
                if (america_nations.count(db->c_nationkey[c_idx])) {
                    order_year[i] = static_cast<int16_t>(epoch_to_year(db->o_orderdate[i]));
                }
            }
        }
    }

    // Scan lineitem: filter on matching partkeys, join with qualifying orders
    // Group by year → sum(volume), sum(france_volume)
    struct YearAgg {
        double total_volume = 0;
        double france_volume = 0;
    };
    std::map<int32_t, YearAgg> groups;

    {
        PROFILE_SCOPE("q8_lineitem_scan_join_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            if (!matching_partkeys.count(db->l_partkey[i])) continue;

            int32_t orderkey = db->l_orderkey[i];
            if (orderkey > db->max_orderkey) continue;
            int32_t o_idx = db->orderkey_to_idx[orderkey];
            if (o_idx < 0) continue;
            int16_t year = order_year[o_idx];
            if (year < 0) continue;

            TRACE_INC(li_emitted);
            double volume = (double)db->l_extendedprice[i] * (100 - db->l_discount[i]); // scale 4 units
            auto& g = groups[year];
            g.total_volume += volume;

            // Check if supplier nation is FRANCE
            int32_t suppkey = db->l_suppkey[i];
            int32_t s_idx = suppkey - 1;
            if (s_idx >= 0 && s_idx < db->n_supplier && db->s_nationkey[s_idx] == france_nk) {
                g.france_volume += volume;
            }
        }
    }
    TRACE_COUNT("q8_rows_scanned", li_scanned);
    TRACE_COUNT("q8_join_rows_emitted", li_emitted);
    TRACE_COUNT("q8_agg_rows_in", li_emitted);
    TRACE_COUNT("q8_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q8_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q8_output");
    write_csv_header(out, {"o_year","mkt_share"});
    for (auto& [year, g] : groups) {
        double mkt_share = (g.total_volume == 0) ? 0.0 : g.france_volume / g.total_volume;
        write_csv_row(out, {
            std::to_string(year),
            fmt_decimal(mkt_share, 15)
        });
    }
    TRACE_COUNT("q8_query_output_rows", (uint64_t)groups.size());
}
