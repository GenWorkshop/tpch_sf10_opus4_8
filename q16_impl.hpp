#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_set>
#include <map>
#include <set>
#include <algorithm>
#include <string>
#include <tuple>

inline void run_q16_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q16_total");
    TRACE_DECL_COUNTER(ps_scanned);
    TRACE_DECL_COUNTER(ps_emitted);
    // Find suppliers whose comment matches '%Customer%Complaints%'
    std::unordered_set<int32_t> bad_suppliers; // suppkeys (1-based)
    for (int32_t i = 0; i < db->n_supplier; i++) {
        const auto& c = db->s_comment[i];
        auto pos = c.find("Customer");
        if (pos != std::string::npos) {
            if (c.find("Complaints", pos + 8) != std::string::npos) {
                bad_suppliers.insert(i + 1);
            }
        }
    }

    // Valid sizes
    std::unordered_set<int32_t> valid_sizes = {22, 18, 10, 14, 50, 7, 6, 25};

    // Filter parts: p_brand != 'Brand#31', p_type NOT LIKE 'SMALL POLISHED%', p_size IN valid_sizes
    // For each qualifying part, collect distinct suppkeys from partsupp (excluding bad suppliers)
    // Group by (p_brand, p_type, p_size) → set of suppkeys

    // First identify qualifying partkeys
    std::unordered_set<int32_t> qual_partkeys; // 1-based
    for (int32_t i = 0; i < db->n_part; i++) {
        if (db->p_brand[i] == "Brand#31") continue;
        const auto& t = db->p_type[i];
        if (t.size() >= 14 && t.substr(0, 14) == "SMALL POLISHED") continue;
        if (!valid_sizes.count(db->p_size[i])) continue;
        qual_partkeys.insert(i + 1);
    }

    // Group key
    using GKey = std::tuple<std::string, std::string, int32_t>;
    std::map<GKey, std::set<int32_t>> groups; // → set of distinct suppkeys

    {
        PROFILE_SCOPE("q16_partsupp_scan_join_agg");
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            TRACE_INC(ps_scanned);
            int32_t pk = db->ps_partkey[i];
            if (!qual_partkeys.count(pk)) continue;
            int32_t sk = db->ps_suppkey[i];
            if (bad_suppliers.count(sk)) continue;

            TRACE_INC(ps_emitted);
            int32_t p_idx = pk - 1;
            GKey key = {db->p_brand[p_idx], db->p_type[p_idx], db->p_size[p_idx]};
            groups[key].insert(sk);
        }
    }
    TRACE_COUNT("q16_rows_scanned", ps_scanned);
    TRACE_COUNT("q16_rows_emitted", ps_emitted);
    TRACE_COUNT("q16_agg_rows_in", ps_emitted);
    TRACE_COUNT("q16_groups_created", (uint64_t)groups.size());

    // Build results
    struct ResultRow {
        std::string p_brand;
        std::string p_type;
        int32_t p_size;
        int64_t supplier_cnt;
    };
    std::vector<ResultRow> results;
    results.reserve(groups.size());
    for (auto& [k, suppset] : groups) {
        results.push_back({std::get<0>(k), std::get<1>(k), std::get<2>(k), (int64_t)suppset.size()});
    }
    TRACE_COUNT("q16_agg_rows_emitted", (uint64_t)results.size());

    // Order by supplier_cnt desc, p_brand, p_type, p_size
    TRACE_COUNT("q16_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q16_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.supplier_cnt != b.supplier_cnt) return a.supplier_cnt > b.supplier_cnt;
            int cmp = a.p_brand.compare(b.p_brand);
            if (cmp != 0) return cmp < 0;
            cmp = a.p_type.compare(b.p_type);
            if (cmp != 0) return cmp < 0;
            return a.p_size < b.p_size;
        });
    }
    TRACE_COUNT("q16_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q16_output");
    write_csv_header(out, {"p_brand","p_type","p_size","supplier_cnt"});
    for (auto& r : results) {
        write_csv_row(out, {
            r.p_brand,
            r.p_type,
            std::to_string(r.p_size),
            std::to_string(r.supplier_cnt)
        });
    }
    TRACE_COUNT("q16_query_output_rows", (uint64_t)results.size());
}
