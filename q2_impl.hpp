#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <climits>

inline void run_q2_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q2_total");
    TRACE_DECL_COUNTER(part_scanned);
    TRACE_DECL_COUNTER(part_emitted);
    TRACE_DECL_COUNTER(probe_rows);
    // Filter: r_name = 'ASIA' → get regionkey
    int32_t asia_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "ASIA") { asia_regionkey = i; break; }
    }

    // Get nationkeys in ASIA
    std::unordered_set<int32_t> asia_nations;
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == asia_regionkey) {
            asia_nations.insert(i);
        }
    }

    // Get suppliers in ASIA nations: suppkey (0-based index) → nationkey
    std::vector<int32_t> asia_suppliers; // indices of suppliers in ASIA
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (asia_nations.count(db->s_nationkey[i])) {
            asia_suppliers.push_back(i);
        }
    }
    // suppkey → bool (is in asia)
    std::vector<bool> supp_in_asia(db->n_supplier, false);
    for (int32_t idx : asia_suppliers) {
        supp_in_asia[idx] = true;
    }

    // Filter parts: p_size = 8 AND p_type LIKE '%TIN'
    std::vector<int32_t> matching_parts; // partkey (1-based), stored as index (0-based)
    {
        PROFILE_SCOPE("q2_part_scan");
        for (int32_t i = 0; i < db->n_part; i++) {
            TRACE_INC(part_scanned);
            if (db->p_size[i] == 8) {
                const auto& t = db->p_type[i];
                if (t.size() >= 3 && t.substr(t.size() - 3) == "TIN") {
                    TRACE_INC(part_emitted);
                    matching_parts.push_back(i);
                }
            }
        }
    }

    // For each matching part, find min supplycost among ASIA suppliers in partsupp
    // Build partkey → partsupp rows index
    // partsupp is sorted by (ps_partkey, ps_suppkey) typically
    // For efficiency, build a map: partkey → list of partsupp row indices
    std::unordered_map<int32_t, std::vector<int32_t>> partkey_to_ps;
    {
        PROFILE_SCOPE("q2_partsupp_build");
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            partkey_to_ps[db->ps_partkey[i]].push_back(i);
        }
    }

    struct Result {
        int64_t s_acctbal;
        int32_t s_idx;
        int32_t n_idx;
        int32_t p_idx;   // 0-based part index (partkey - 1)
    };

    std::vector<Result> results;

    {
        PROFILE_SCOPE("q2_join_probe");
        for (int32_t p_idx : matching_parts) {
            int32_t partkey = p_idx + 1; // partkeys are 1-based
            auto it = partkey_to_ps.find(partkey);
            if (it == partkey_to_ps.end()) continue;

            // Find min supplycost among ASIA suppliers for this part
            int64_t min_cost = INT64_MAX;
            for (int32_t ps_row : it->second) {
                TRACE_INC(probe_rows);
                int32_t suppkey = db->ps_suppkey[ps_row];
                int32_t s_idx = suppkey - 1; // 0-based
                if (s_idx >= 0 && s_idx < db->n_supplier && supp_in_asia[s_idx]) {
                    if (db->ps_supplycost[ps_row] < min_cost) {
                        min_cost = db->ps_supplycost[ps_row];
                    }
                }
            }

            if (min_cost == INT64_MAX) continue;

            // Now find all (supplier, partsupp) combos that match min_cost in ASIA
            for (int32_t ps_row : it->second) {
                int32_t suppkey = db->ps_suppkey[ps_row];
                int32_t s_idx = suppkey - 1;
                if (s_idx >= 0 && s_idx < db->n_supplier && supp_in_asia[s_idx]) {
                    if (db->ps_supplycost[ps_row] == min_cost) {
                        results.push_back({db->s_acctbal[s_idx], s_idx, db->s_nationkey[s_idx], p_idx});
                    }
                }
            }
        }
    }

    TRACE_COUNT("q2_part_rows_scanned", part_scanned);
    TRACE_COUNT("q2_part_rows_emitted", part_emitted);
    TRACE_COUNT("q2_probe_rows_in", probe_rows);
    TRACE_COUNT("q2_join_rows_emitted", (uint64_t)results.size());

    // Order by: s_acctbal desc, n_name, s_name, p_partkey
    {
        PROFILE_SCOPE("q2_sort");
        std::sort(results.begin(), results.end(), [&](const Result& a, const Result& b) {
            if (a.s_acctbal != b.s_acctbal) return a.s_acctbal > b.s_acctbal;
            int cmp = db->n_name[a.n_idx].compare(db->n_name[b.n_idx]);
            if (cmp != 0) return cmp < 0;
            cmp = db->s_name[a.s_idx].compare(db->s_name[b.s_idx]);
            if (cmp != 0) return cmp < 0;
            return (a.p_idx + 1) < (b.p_idx + 1);
        });
    }
    TRACE_COUNT("q2_sort_rows_in", (uint64_t)results.size());
    TRACE_COUNT("q2_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q2_output");
    write_csv_header(out, {"s_acctbal","s_name","n_name","p_partkey","p_mfgr","s_address","s_phone","s_comment"});

    for (auto& r : results) {
        write_csv_row(out, {
            fmt_money(r.s_acctbal, 2),
            csv_quote(db->s_name[r.s_idx]),
            csv_quote(db->n_name[r.n_idx]),
            std::to_string(r.p_idx + 1),
            csv_quote(db->p_mfgr[r.p_idx]),
            csv_quote(db->s_address[r.s_idx]),
            csv_quote(db->s_phone[r.s_idx]),
            csv_quote(db->s_comment[r.s_idx])
        });
    }
    TRACE_COUNT("q2_query_output_rows", (uint64_t)results.size());
}
