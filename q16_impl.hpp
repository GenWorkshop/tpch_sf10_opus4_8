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
    {
        PROFILE_SCOPE("q16_bad_supplier");
        for (int32_t i = 0; i < db->n_supplier; i++) {
        const std::string& c = db->s_comment[i];
        auto pos = c.find("Customer");
        if (pos != std::string::npos &&
            c.find("Complaints", pos + 8) != std::string::npos) {
            bad_supplier[i + 1] = 1;
        }
    }
    }

    static const bool size_ok_arr[51] = {
        false,false,false,false,false,false,true,true,false,false,true,   // 0..10
        false,false,false,true,false,false,false,true,false,false,        // 11..20
        false,true,false,false,true,false,false,false,false,false,        // 21..30
        false,false,false,false,false,false,false,false,false,false,      // 31..40
        false,false,false,false,false,false,false,false,false,true        // 41..50
    };

    // Part filter + group assignment.
    // part_gid[partkey] = group id (>=0) or -1 if part filtered out.
    struct GroupInfo {
        std::string p_brand;
        std::string p_type;
        int32_t p_size;
    };
    std::vector<GroupInfo> groups;
    groups.reserve(1 << 15);

    // Group key is packed as ((brand_code*256 + type_id)*64 + size) into a flat
    // array, avoiding hashing of a long concatenated string per qualifying part.
    // type_id is interned through a tiny map (<=150 distinct TPC-H types, stays
    // hot in cache).  brand_code is parsed directly from "Brand#MN".
    std::unordered_map<std::string, int32_t> type_intern;
    type_intern.reserve(256);
    std::vector<int32_t> group_of(1 << 21, -1);

    std::vector<int32_t> part_gid(db->n_part + 1, -1);

    {
    PROFILE_SCOPE("q16_part_filter");
    for (int32_t i = 0; i < db->n_part; i++) {
        int32_t sz = db->p_size[i];
        if (!size_ok_arr[sz]) continue;
        const std::string& br = db->p_brand[i];
        if (br == "Brand#31") continue;
        const std::string& ty = db->p_type[i];
        if (ty.size() >= 14 && ty.compare(0, 14, "SMALL POLISHED") == 0) continue;

        int32_t bc = (int32_t)(br[6] - '0') * 10 + (int32_t)(br[7] - '0');
        auto ins = type_intern.try_emplace(ty, (int32_t)type_intern.size());
        int32_t tid = ins.first->second;
        int32_t packed = (bc * 256 + tid) * 64 + sz;

        int32_t gid = group_of[packed];
        if (gid < 0) {
            gid = (int32_t)groups.size();
            group_of[packed] = gid;
            groups.push_back({br, ty, sz});
        }
        part_gid[i + 1] = gid;
    }
    }

    // Partsupp scan/join: collect surviving (gid, suppkey) pairs.
    const int32_t G = (int32_t)groups.size();
    std::vector<int32_t> pair_gid;
    std::vector<int32_t> pair_sk;
    pair_gid.reserve(1u << 21);
    pair_sk.reserve(1u << 21);
    std::vector<int32_t> gcount(G + 1, 0);
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
            pair_gid.push_back(gid);
            pair_sk.push_back(sk);
            gcount[gid + 1]++;
        }
    }
    TRACE_COUNT("q16_rows_scanned", ps_scanned);
    TRACE_COUNT("q16_rows_emitted", ps_emitted);
    TRACE_COUNT("q16_agg_rows_in", ps_emitted);

    // Distinct count per group via counting-sort bucketing + stamp dedup
    // (avoids an O(n log n) comparison sort over ~1.2M pairs).
    std::vector<int64_t> supplier_cnt(G, 0);
    {
        PROFILE_SCOPE("q16_distinct_count");
        const size_t m = pair_gid.size();
        for (int32_t g = 0; g < G; g++) gcount[g + 1] += gcount[g];
        std::vector<int32_t> offset(gcount.begin(), gcount.end());
        std::vector<int32_t> bucket(m);
        for (size_t i = 0; i < m; i++) {
            bucket[offset[pair_gid[i]]++] = pair_sk[i];
        }
        std::vector<int32_t> seen(db->n_supplier + 1, -1);
        for (int32_t g = 0; g < G; g++) {
            int64_t c = 0;
            for (int32_t k = gcount[g]; k < gcount[g + 1]; k++) {
                int32_t sk = bucket[k];
                if (seen[sk] != g) { seen[sk] = g; c++; }
            }
            supplier_cnt[g] = c;
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
