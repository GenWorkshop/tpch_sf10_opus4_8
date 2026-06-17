#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// q21: Suppliers who kept orders waiting
// supplier(RUSSIA) join lineitem l1 join orders(orderstatus='F')
// WHERE l1.receiptdate > l1.commitdate
// AND EXISTS (l2 with same orderkey, different suppkey)
// AND NOT EXISTS (l3 with same orderkey, different suppkey, l3.receiptdate > l3.commitdate)
// Group by s_name, count(*) as numwait
// Order by numwait DESC, s_name ASC

static void run_q21(Database* db, const std::string& run_nr) {
    // Find RUSSIA nationkey
    int32_t russia_nk = -1;
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        if (db->n_name[i] == "RUSSIA") { russia_nk = db->n_nationkey[i]; break; }
    }

    // Russia suppliers set
    std::unordered_set<int32_t> russia_supps;
    for (int32_t sk : db->nation_to_suppliers[russia_nk]) {
        russia_supps.insert(sk);
    }

    // Orders with status 'F'
    std::unordered_set<int32_t> f_orders;
    TRACE_DECL(orders_scanned);
    {
        PROFILE_SCOPE("q21_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(orders_scanned);
            if (db->o_orderstatus[i] == 'F') {
                f_orders.insert(db->o_orderkey[i]);
            }
        }
    }
    TRACE_COUNT("q21_orders_scanned", orders_scanned);
    TRACE_COUNT("q21_f_orders", f_orders.size());

    // For each orderkey, collect (suppkey, late?) pairs
    // We need to know per order: which suppliers are present, and which are late
    // Use the orderkey index to iterate lineitem by order
    struct OrderLineInfo {
        std::vector<int32_t> suppkeys;
        std::vector<bool> is_late; // receiptdate > commitdate
    };

    // Build per-order info only for F orders that have at least one Russia supplier
    // First pass: find orders involving Russia suppliers with late delivery
    std::unordered_map<int32_t, int64_t> supp_numwait;

    TRACE_DECL(orders_probed);
    TRACE_DECL(lineitems_visited);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q21_lineitem_group_probe");
        // Iterate through lineitem grouped by orderkey using the index
        for (int32_t ok = 0; ok <= db->max_orderkey; ok++) {
            int32_t start = db->orderkey_to_li_start[ok];
            int32_t end = db->orderkey_to_li_end[ok];
            if (start >= end) continue;
            if (!f_orders.count(ok)) continue;
            TRACE_INC(orders_probed);

            // Collect suppkeys and lateness for this order
            struct LI { int32_t suppkey; bool late; };
            std::vector<LI> lis;
            lis.reserve(end - start);
            for (int32_t idx = start; idx < end; idx++) {
                TRACE_INC(lineitems_visited);
                lis.push_back({db->l_suppkey[idx], db->l_receiptdate[idx] > db->l_commitdate[idx]});
            }

            // For each lineitem l1 from a Russia supplier that is late:
            for (auto& l1 : lis) {
                if (!russia_supps.count(l1.suppkey)) continue;
                if (!l1.late) continue;

                // EXISTS: another suppkey in the same order
                bool exists_other = false;
                for (auto& l2 : lis) {
                    if (l2.suppkey != l1.suppkey) { exists_other = true; break; }
                }
                if (!exists_other) continue;

                // NOT EXISTS: no other suppkey that is also late
                bool exists_other_late = false;
                for (auto& l3 : lis) {
                    if (l3.suppkey != l1.suppkey && l3.late) { exists_other_late = true; break; }
                }
                if (exists_other_late) continue;

                TRACE_INC(join_rows_emitted);
                supp_numwait[l1.suppkey]++;
            }
        }
    }
    TRACE_COUNT("q21_orders_probed", orders_probed);
    TRACE_COUNT("q21_lineitems_visited", lineitems_visited);
    TRACE_COUNT("q21_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q21_groups_created", supp_numwait.size());

    // Collect results
    struct Result { std::string name; int64_t numwait; };
    std::vector<Result> results;
    for (auto& [sk, nw] : supp_numwait) {
        results.push_back({db->s_name[sk], nw});
    }

    {
        PROFILE_SCOPE("q21_sort");
        // Sort by numwait DESC, s_name ASC
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            if (a.numwait != b.numwait) return a.numwait > b.numwait;
            return a.name < b.name;
        });
    }
    TRACE_COUNT("q21_sort_rows_in", results.size());
    TRACE_COUNT("q21_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q21_output");
    std::ostringstream oss;
    write_csv_header(oss, {"s_name","numwait"});
    for (auto& r : results) {
        write_csv_row(oss, {csv_quote(r.name), std::to_string(r.numwait)});
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q21_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
