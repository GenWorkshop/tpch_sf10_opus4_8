#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp"  // date_to_epoch
#include "q7_impl.hpp"  // epoch_to_year
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <algorithm>

inline void run_q9_impl(Database* db, std::ostream& out) {
    // Filter: p_name like '%rosy%'
    std::unordered_set<int32_t> matching_partkeys; // 1-based
    for (int32_t i = 0; i < db->n_part; i++) {
        if (db->p_name[i].find("rosy") != std::string::npos) {
            matching_partkeys.insert(i + 1);
        }
    }

    // Build partsupp lookup: (partkey, suppkey) → supplycost
    std::unordered_map<int64_t, int64_t> ps_cost; // key = partkey*1000000LL + suppkey
    for (int32_t i = 0; i < db->n_partsupp; i++) {
        int64_t key = (int64_t)db->ps_partkey[i] * 1000000LL + db->ps_suppkey[i];
        ps_cost[key] = db->ps_supplycost[i];
    }

    // Group by (nation_name, o_year) → sum(amount)
    // amount = l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity
    // In scale terms: extprice(s2) * (100-disc(s2)) = scale 4
    //                 supplycost(s2) * quantity(s2) = scale 4
    //                 amount = scale 4 difference
    struct Key {
        int32_t nation_key;
        int32_t year;
        bool operator<(const Key& o) const {
            if (nation_key != o.nation_key) return nation_key < o.nation_key;
            return year > o.year; // desc
        }
    };
    std::map<Key, __int128> groups;

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        int32_t partkey = db->l_partkey[i];
        if (!matching_partkeys.count(partkey)) continue;

        int32_t suppkey = db->l_suppkey[i];
        int32_t s_idx = suppkey - 1;
        if (s_idx < 0 || s_idx >= db->n_supplier) continue;

        // Get supplycost from partsupp
        int64_t ps_key = (int64_t)partkey * 1000000LL + suppkey;
        auto it = ps_cost.find(ps_key);
        if (it == ps_cost.end()) continue;
        int64_t supplycost = it->second;

        // Get order year
        int32_t orderkey = db->l_orderkey[i];
        if (orderkey > db->max_orderkey) continue;
        int32_t o_idx = db->orderkey_to_idx[orderkey];
        if (o_idx < 0) continue;
        int32_t year = epoch_to_year(db->o_orderdate[o_idx]);

        // Get supplier nation
        int32_t nation_key = db->s_nationkey[s_idx];

        // amount = extprice * (100 - disc) - supplycost * quantity (all scale 4)
        __int128 amount = (__int128)db->l_extendedprice[i] * (100 - db->l_discount[i])
                        - (__int128)supplycost * db->l_quantity[i];

        groups[{nation_key, year}] += amount;
    }

    // Sort: by nation name asc, then year desc (already handled by Key::operator<)
    // But we need to sort by nation *name* not key, so let's re-sort
    struct ResultRow {
        std::string nation;
        int32_t year;
        __int128 sum_profit;
    };
    std::vector<ResultRow> results;
    for (auto& [k, v] : groups) {
        results.push_back({db->n_name[k.nation_key], k.year, v});
    }
    std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
        int cmp = a.nation.compare(b.nation);
        if (cmp != 0) return cmp < 0;
        return a.year > b.year;
    });

    write_csv_header(out, {"nation","o_year","sum_profit"});
    for (auto& r : results) {
        write_csv_row(out, {
            r.nation,
            std::to_string(r.year),
            fmt_money(static_cast<long long>(r.sum_profit), 4)
        });
    }
}
