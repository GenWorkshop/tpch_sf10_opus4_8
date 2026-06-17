#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstdint>
#include <string>

inline void run_q16_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q16_total");
    TRACE_DECL_COUNTER(ps_scanned);
    TRACE_DECL_COUNTER(ps_emitted);

    // Bad suppliers: s_comment LIKE '%Customer%Complaints%' -> dense bool by suppkey
    std::vector<uint8_t> bad_supplier(db->n_supplier + 1, 0);
    for (int32_t i = 0; i < db->n_supplier; i++) {
        const std::string& c = db->s_comment[i];
        auto pos = c.find("Customer");
        if (pos != std::string::npos &&
            c.find("Complaints", pos + 8) != std::string::npos) {
            bad_supplier[i + 1] = 1;
        }
    }

    auto is_valid_size = [](int32_t s) -> bool {
        switch (s) {
            case 6: case 7: case 10: case 14:
            case 18: case 22: case 25: case 50: return true;
            default: return false;
        }
    };

    // Part filter + group assignment.
    // part_gid[partkey] = group id (>=0) or -1 if part filtered out.
    struct GroupInfo {
        std::string p_brand;
        std::string p_type;
        int32_t p_size;
    };
    std::vector<GroupInfo> groups;
    std::unordered_map<std::string, int32_t> group_id;
    group_id.reserve(1 << 16);

    std::vector<int32_t> part_gid(db->n_part + 1, -1);

    std::string key;
    for (int32_t i = 0; i < db->n_part; i++) {
        int32_t sz = db->p_size[i];
        if (!is_valid_size(sz)) continue;
        const std::string& br = db->p_brand[i];
        if (br == "Brand#31") continue;
        const std::string& ty = db->p_type[i];
        if (ty.size() >= 14 && ty.compare(0, 14, "SMALL POLISHED") == 0) continue;

        key.clear();
        key.append(br);
        key.push_back('\0');
        key.append(ty);
        key.push_back('\0');
        key.append(reinterpret_cast<const char*>(&sz), sizeof(sz));

        auto it = group_id.find(key);
        int32_t gid;
        if (it == group_id.end()) {
            gid = (int32_t)groups.size();
            group_id.emplace(key, gid);
            groups.push_back({br, ty, sz});
        } else {
            gid = it->second;
        }
        part_gid[i + 1] = gid;
    }

    // Partsupp scan/join: emit packed (gid, suppkey) keys for distinct counting.
    // high 32 bits = gid, low 32 bits = suppkey.
    std::vector<uint64_t> pairs;
    pairs.reserve(db->n_partsupp);
    {
        PROFILE_SCOPE("q16_partsupp_scan_join_agg");
        const int32_t* psp = db->ps_partkey.data();
        const int32_t* pss = db->ps_suppkey.data();
        const int32_t* pg = part_gid.data();
        const uint8_t* bad = bad_supplier.data();
        int32_t n = db->n_partsupp;
        for (int32_t i = 0; i < n; i++) {
            TRACE_INC(ps_scanned);
            int32_t gid = pg[psp[i]];
            if (gid < 0) continue;
            int32_t sk = pss[i];
            if (bad[sk]) continue;
            TRACE_INC(ps_emitted);
            pairs.push_back(((uint64_t)(uint32_t)gid << 32) | (uint32_t)sk);
        }
    }
    TRACE_COUNT("q16_rows_scanned", ps_scanned);
    TRACE_COUNT("q16_rows_emitted", ps_emitted);
    TRACE_COUNT("q16_agg_rows_in", ps_emitted);

    // Distinct count per group via sort + unique scan.
    std::sort(pairs.begin(), pairs.end());
    std::vector<int64_t> supplier_cnt(groups.size(), 0);
    {
        size_t m = pairs.size();
        for (size_t i = 0; i < m; ) {
            uint64_t v = pairs[i];
            supplier_cnt[v >> 32]++;
            size_t j = i + 1;
            while (j < m && pairs[j] == v) j++;
            i = j;
        }
    }
    TRACE_COUNT("q16_groups_created", (uint64_t)groups.size());

    // Build results, dropping empty groups (no qualifying partsupp rows).
    struct ResultRow {
        const std::string* p_brand;
        const std::string* p_type;
        int32_t p_size;
        int64_t supplier_cnt;
    };
    std::vector<ResultRow> results;
    results.reserve(groups.size());
    for (size_t g = 0; g < groups.size(); g++) {
        if (supplier_cnt[g] == 0) continue;
        results.push_back({&groups[g].p_brand, &groups[g].p_type,
                           groups[g].p_size, supplier_cnt[g]});
    }
    TRACE_COUNT("q16_agg_rows_emitted", (uint64_t)results.size());

    TRACE_COUNT("q16_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q16_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.supplier_cnt != b.supplier_cnt) return a.supplier_cnt > b.supplier_cnt;
            int cmp = a.p_brand->compare(*b.p_brand);
            if (cmp != 0) return cmp < 0;
            cmp = a.p_type->compare(*b.p_type);
            if (cmp != 0) return cmp < 0;
            return a.p_size < b.p_size;
        });
    }
    TRACE_COUNT("q16_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q16_output");
    write_csv_header(out, {"p_brand","p_type","p_size","supplier_cnt"});
    for (auto& r : results) {
        write_csv_row(out, {
            *r.p_brand,
            *r.p_type,
            std::to_string(r.p_size),
            std::to_string(r.supplier_cnt)
        });
    }
    TRACE_COUNT("q16_query_output_rows", (uint64_t)results.size());
}
