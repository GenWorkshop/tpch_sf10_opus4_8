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
    // o_orderdate >= '1993-09-01' AND o_orderdate < '1993-12-01'
    const Date date_lo = date_to_epoch(1993, 9, 1);
    const Date date_hi = date_to_epoch(1993, 12, 1);

    // Build set of orderkeys that have at least one lineitem with commitdate < receiptdate
    std::vector<bool> order_has_late(db->max_orderkey + 1, false);

    if (db->lineitem_sorted_by_orderkey) {
        // Use CSR index
        for (int32_t i = 0; i < db->n_orders; i++) {
            int32_t ok = db->o_orderkey[i];
            if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
                int64_t start = db->orderkey_lineitem_start[ok];
                int64_t end = db->orderkey_lineitem_end[ok];
                for (int64_t j = start; j < end; j++) {
                    if (db->l_commitdate[j] < db->l_receiptdate[j]) {
                        order_has_late[ok] = true;
                        break;
                    }
                }
            }
        }
    } else {
        // Scan all lineitem to find orders with late receipts
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            if (db->l_commitdate[i] < db->l_receiptdate[i]) {
                int32_t ok = db->l_orderkey[i];
                if (ok <= db->max_orderkey) {
                    order_has_late[ok] = true;
                }
            }
        }
    }

    // Count by orderpriority
    std::map<std::string, int64_t> counts;
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
            int32_t ok = db->o_orderkey[i];
            if (order_has_late[ok]) {
                counts[db->o_orderpriority[i]]++;
            }
        }
    }

    write_csv_header(out, {"o_orderpriority","order_count"});
    for (auto& [prio, cnt] : counts) {
        write_csv_row(out, {prio, std::to_string(cnt)});
    }
}
