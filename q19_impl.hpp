#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <unordered_set>
#include <string>

inline void run_q19_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q19_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Three OR conditions, all require:
    //   p_partkey = l_partkey
    //   l_shipmode in ('AIR', 'AIR REG')
    //   l_shipinstruct = 'DELIVER IN PERSON'
    
    // Quantities are scale 2
    // Group 1: Brand#14, SM containers, qty 1-11, size 1-5
    // Group 2: Brand#15, MED containers, qty 17-27, size 1-10
    // Group 3: Brand#35, LG containers, qty 28-38, size 1-15

    __int128 revenue = 0;

    {
        PROFILE_SCOPE("q19_lineitem_scan_join_filter");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            // Common filters first
            if (db->l_shipinstruct[i] != "DELIVER IN PERSON") continue;
            const auto& mode = db->l_shipmode[i];
            if (mode != "AIR" && mode != "AIR REG") continue;

            int32_t partkey = db->l_partkey[i];
            int32_t p_idx = partkey - 1;
            if (p_idx < 0 || p_idx >= db->n_part) continue;

            int64_t qty = db->l_quantity[i]; // scale 2
            int32_t psize = db->p_size[p_idx];
            const auto& brand = db->p_brand[p_idx];
            const auto& container = db->p_container[p_idx];

            bool match = false;

            // Group 1: Brand#14, SM containers, qty [1,11] (scale 2: [100, 1100]), size [1,5]
            if (brand == "Brand#14" &&
                (container == "SM CASE" || container == "SM BOX" || container == "SM PACK" || container == "SM PKG") &&
                qty >= 100 && qty <= 1100 &&
                psize >= 1 && psize <= 5) {
                match = true;
            }
            // Group 2: Brand#15, MED containers, qty [17,27] (scale 2: [1700, 2700]), size [1,10]
            else if (brand == "Brand#15" &&
                (container == "MED BAG" || container == "MED BOX" || container == "MED PKG" || container == "MED PACK") &&
                qty >= 1700 && qty <= 2700 &&
                psize >= 1 && psize <= 10) {
                match = true;
            }
            // Group 3: Brand#35, LG containers, qty [28,38] (scale 2: [2800, 3800]), size [1,15]
            else if (brand == "Brand#35" &&
                (container == "LG CASE" || container == "LG BOX" || container == "LG PACK" || container == "LG PKG") &&
                qty >= 2800 && qty <= 3800 &&
                psize >= 1 && psize <= 15) {
                match = true;
            }

            if (match) {
                TRACE_INC(li_emitted);
                revenue += (__int128)db->l_extendedprice[i] * (100 - db->l_discount[i]);
            }
        }
    }
    TRACE_COUNT("q19_rows_scanned", li_scanned);
    TRACE_COUNT("q19_rows_emitted", li_emitted);
    TRACE_COUNT("q19_agg_rows_in", li_emitted);

    PROFILE_SCOPE("q19_output");
    write_csv_header(out, {"revenue"});
    write_csv_row(out, {fmt_money(static_cast<long long>(revenue), 4)});
    TRACE_COUNT("q19_query_output_rows", 1);
}
