#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <string>

inline void run_q14_impl(Database* db, std::ostream& out) {
    // l_shipdate >= '1995-05-01' AND l_shipdate < '1995-06-01'
    const Date date_lo = date_to_epoch(1995, 5, 1);
    const Date date_hi = date_to_epoch(1995, 6, 1);

    double promo_sum = 0.0;
    double total_sum = 0.0;

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        if (db->l_shipdate[i] >= date_lo && db->l_shipdate[i] < date_hi) {
            double rev = (double)db->l_extendedprice[i] * (100 - db->l_discount[i]);
            total_sum += rev;

            // Check if part type starts with "PROMO"
            int32_t partkey = db->l_partkey[i];
            int32_t p_idx = partkey - 1;
            if (p_idx >= 0 && p_idx < db->n_part) {
                const auto& ptype = db->p_type[p_idx];
                if (ptype.size() >= 5 && ptype.substr(0, 5) == "PROMO") {
                    promo_sum += rev;
                }
            }
        }
    }

    double promo_revenue = (total_sum == 0.0) ? 0.0 : 100.0 * promo_sum / total_sum;

    write_csv_header(out, {"promo_revenue"});
    write_csv_row(out, {fmt_decimal(promo_revenue, 15)});
}
