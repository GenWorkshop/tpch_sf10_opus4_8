#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>

inline void run_q6_impl(Database* db, std::ostream& out) {
    // l_shipdate >= '1993-01-01' AND l_shipdate < '1994-01-01'
    // l_discount BETWEEN 0.07 AND 0.09 (stored as scale 2: 7 and 9)
    // l_quantity < 24 (stored as scale 2: 2400)
    const Date date_lo = date_to_epoch(1993, 1, 1);
    const Date date_hi = date_to_epoch(1994, 1, 1);
    const int64_t disc_lo = 7;   // 0.08 - 0.01 = 0.07, scale 2
    const int64_t disc_hi = 9;   // 0.08 + 0.01 = 0.09, scale 2
    const int64_t qty_limit = 2400; // 24 in scale 2

    // sum(l_extendedprice * l_discount) → scale 4 (s2 * s2)
    __int128 revenue = 0;

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        if (db->l_shipdate[i] >= date_lo && db->l_shipdate[i] < date_hi &&
            db->l_discount[i] >= disc_lo && db->l_discount[i] <= disc_hi &&
            db->l_quantity[i] < qty_limit) {
            revenue += (__int128)db->l_extendedprice[i] * db->l_discount[i];
        }
    }

    write_csv_header(out, {"revenue"});
    write_csv_row(out, {fmt_money(static_cast<long long>(revenue), 4)});
}
