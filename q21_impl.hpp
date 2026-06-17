#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

inline void run_q21_impl(Database* db, std::ostream& out) {
    // n_name = 'RUSSIA' → nationkey
    int32_t russia_nk = db->nation_name_to_key["RUSSIA"];

    // Suppliers in RUSSIA
    std::unordered_set<int32_t> russia_suppkeys; // 1-based
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (db->s_nationkey[i] == russia_nk) {
            russia_suppkeys.insert(i + 1);
        }
    }

    // Orders with o_orderstatus = 'F'
    std::vector<bool> order_is_F(db->max_orderkey + 1, false);
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderstatus[i] == 'F') {
            order_is_F[db->o_orderkey[i]] = true;
        }
    }

    // For each orderkey, collect (suppkey, is_late) pairs
    // We need:
    // l1: RUSSIA supplier, receipt > commit (late), order is F
    // EXISTS l2: same order, different supplier
    // NOT EXISTS l3: same order, different supplier, also late

    // Build per-order info: set of suppkeys and set of late suppkeys
    struct OrderInfo {
        std::vector<int32_t> suppkeys;
        std::unordered_set<int32_t> late_suppkeys; // those with receipt > commit
    };

    std::unordered_map<int32_t, OrderInfo> order_info;

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        int32_t ok = db->l_orderkey[i];
        if (ok > db->max_orderkey || !order_is_F[ok]) continue;

        auto& info = order_info[ok];
        int32_t sk = db->l_suppkey[i];
        info.suppkeys.push_back(sk);
        if (db->l_receiptdate[i] > db->l_commitdate[i]) {
            info.late_suppkeys.insert(sk);
        }
    }

    // Count numwait per RUSSIA supplier
    std::unordered_map<int32_t, int64_t> supp_numwait; // suppkey → count

    for (auto& [ok, info] : order_info) {
        // For each RUSSIA supplier that is late on this order
        for (int32_t sk : info.late_suppkeys) {
            if (!russia_suppkeys.count(sk)) continue;

            // EXISTS: another supplier on the same order (different suppkey)
            bool has_other = false;
            for (int32_t s2 : info.suppkeys) {
                if (s2 != sk) { has_other = true; break; }
            }
            if (!has_other) continue;

            // NOT EXISTS: no other supplier that is also late
            bool other_late = false;
            for (int32_t s2 : info.late_suppkeys) {
                if (s2 != sk) { other_late = true; break; }
            }
            if (other_late) continue;

            supp_numwait[sk]++;
        }
    }

    // Build results
    struct ResultRow {
        std::string s_name;
        int64_t numwait;
    };
    std::vector<ResultRow> results;
    for (auto& [sk, nw] : supp_numwait) {
        results.push_back({db->s_name[sk - 1], nw});
    }

    // Order by numwait desc, s_name asc
    std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
        if (a.numwait != b.numwait) return a.numwait > b.numwait;
        return a.s_name < b.s_name;
    });

    write_csv_header(out, {"s_name","numwait"});
    for (auto& r : results) {
        write_csv_row(out, {csv_quote(r.s_name), std::to_string(r.numwait)});
    }
}
