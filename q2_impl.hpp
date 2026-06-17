#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_set>
#include <climits>

// q2: Find suppliers in ASIA region offering minimum cost for parts with
// p_size=8 and p_type LIKE '%TIN', ordered by s_acctbal desc, n_name, s_name, p_partkey

static void run_q2(Database* db, const std::string& run_nr) {
    // Find ASIA region key
    int32_t asia_regionkey = -1;
    for (size_t i = 0; i < db->r_regionkey.size(); i++) {
        if (db->r_name[i] == "ASIA") {
            asia_regionkey = db->r_regionkey[i];
            break;
        }
    }

    // ASIA supplier bitmap indexed by suppkey (array lookup, no hashing)
    std::vector<char> is_asia(db->supplier_count + 1, 0);
    for (int32_t nk : db->region_to_nation_keys[asia_regionkey]) {
        for (int32_t sk : db->nation_to_suppliers[nk]) {
            is_asia[sk] = 1;
        }
    }

    // For each part matching p_size=8 and p_type LIKE '%TIN':
    // Find the minimum ps_supplycost among ASIA suppliers
    // Then collect all (part, supplier) pairs matching that minimum

    struct Result {
        int64_t s_acctbal;
        std::string s_name;
        std::string n_name;
        int32_t p_partkey;
        std::string p_mfgr;
        std::string s_address;
        std::string s_phone;
        std::string s_comment;
    };

    std::vector<Result> results;

    // Step 1: filter parts (p_size=8, p_type LIKE '%TIN') into a bitmap.
    // This is the most selective predicate, so we use it to prune partsupp.
    std::vector<char> is_match(db->part_count + 1, 0);
    TRACE_DECL(parts_scanned);
    TRACE_DECL(parts_matched);
    {
        PROFILE_SCOPE("q2_part_filter");
        for (int32_t pk = 1; pk <= db->part_count; pk++) {
            TRACE_INC(parts_scanned);
            if (db->p_size[pk] != 8) continue;
            const std::string& ptype = db->p_type[pk];
            if (ptype.size() < 3 || ptype.compare(ptype.size() - 3, 3, "TIN") != 0) continue;
            is_match[pk] = 1;
            TRACE_INC(parts_matched);
        }
    }
    TRACE_COUNT("q2_parts_scanned", parts_scanned);
    TRACE_COUNT("q2_parts_matched", parts_matched);

    // Step 2: scan partsupp once. The matching-part bitmap check is extremely
    // selective (cheap array lookup), pruning the vast majority of rows before
    // the ASIA supplier check. Surviving rows are grouped per matching part.
    struct PSEntry { int32_t suppkey; int64_t supplycost; };
    std::unordered_map<int32_t, std::vector<PSEntry>> ps_by_part;
    TRACE_DECL(ps_scanned);
    {
        PROFILE_SCOPE("q2_partsupp_probe");
        for (int64_t i = 0; i < db->partsupp_count; i++) {
            TRACE_INC(ps_scanned);
            int32_t pk = db->ps_partkey[i];
            if (!is_match[pk]) continue;
            int32_t sk = db->ps_suppkey[i];
            if (!is_asia[sk]) continue;
            ps_by_part[pk].push_back({sk, db->ps_supplycost[i]});
        }
    }
    TRACE_COUNT("q2_partsupp_scanned", ps_scanned);
    TRACE_COUNT("q2_build_rows", ps_by_part.size());

    // Step 3: for each matching part, pick the min supplycost and emit all
    // suppliers that achieve it.
    {
        PROFILE_SCOPE("q2_minagg_emit");
        for (auto& kv : ps_by_part) {
            int32_t pk = kv.first;
            auto& entries = kv.second;
            int64_t min_cost = LLONG_MAX;
            for (auto& e : entries) {
                if (e.supplycost < min_cost) min_cost = e.supplycost;
            }
            for (auto& e : entries) {
                if (e.supplycost == min_cost) {
                    int32_t sk = e.suppkey;
                    int32_t nk = db->s_nationkey[sk];
                    results.push_back({
                        db->s_acctbal[sk],
                        db->s_name[sk],
                        db->nationkey_to_name[nk],
                        pk,
                        db->p_mfgr[pk],
                        db->s_address[sk],
                        db->s_phone[sk],
                        db->s_comment[sk]
                    });
                }
            }
        }
    }
    TRACE_COUNT("q2_join_rows_emitted", results.size());

    {
        PROFILE_SCOPE("q2_sort");
        // Sort: s_acctbal DESC, n_name ASC, s_name ASC, p_partkey ASC
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            if (a.s_acctbal != b.s_acctbal) return a.s_acctbal > b.s_acctbal;
            if (a.n_name != b.n_name) return a.n_name < b.n_name;
            if (a.s_name != b.s_name) return a.s_name < b.s_name;
            return a.p_partkey < b.p_partkey;
        });
    }
    TRACE_COUNT("q2_sort_rows_in", results.size());
    TRACE_COUNT("q2_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q2_output");
    std::ostringstream oss;
    write_csv_header(oss, {"s_acctbal","s_name","n_name","p_partkey","p_mfgr","s_address","s_phone","s_comment"});

    for (auto& r : results) {
        write_csv_row(oss, {
            fmt_decimal(r.s_acctbal / 100.0, 2),
            csv_quote(r.s_name),
            csv_quote(r.n_name),
            std::to_string(r.p_partkey),
            csv_quote(r.p_mfgr),
            csv_quote(r.s_address),
            csv_quote(r.s_phone),
            csv_quote(r.s_comment)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q2_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
