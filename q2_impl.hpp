#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
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

    // Find nation keys in ASIA
    std::unordered_set<int32_t> asia_nations;
    for (int32_t nk : db->region_to_nation_keys[asia_regionkey]) {
        asia_nations.insert(nk);
    }

    // Find suppliers in ASIA (suppkey -> true)
    std::unordered_set<int32_t> asia_suppliers;
    for (int32_t nk : db->region_to_nation_keys[asia_regionkey]) {
        for (int32_t sk : db->nation_to_suppliers[nk]) {
            asia_suppliers.insert(sk);
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

    // Build partsupp index: partkey -> list of (suppkey, supplycost) for ASIA suppliers
    // partsupp is stored flat, so we need to scan it
    // Build a map: partkey -> vector of {suppkey, supplycost}
    struct PSEntry { int32_t suppkey; int64_t supplycost; };
    std::unordered_map<int32_t, std::vector<PSEntry>> ps_by_part;
    for (int64_t i = 0; i < db->partsupp_count; i++) {
        int32_t sk = db->ps_suppkey[i];
        if (asia_suppliers.count(sk)) {
            ps_by_part[db->ps_partkey[i]].push_back({sk, db->ps_supplycost[i]});
        }
    }

    // Scan parts
    for (int32_t pk = 1; pk < (int32_t)db->p_size.size(); pk++) {
        if (db->p_size[pk] != 8) continue;
        const std::string& ptype = db->p_type[pk];
        // Check LIKE '%TIN' - ends with "TIN"
        if (ptype.size() < 3 || ptype.substr(ptype.size() - 3) != "TIN") continue;

        auto it = ps_by_part.find(pk);
        if (it == ps_by_part.end()) continue;

        // Find min supplycost for this part among ASIA suppliers
        int64_t min_cost = LLONG_MAX;
        for (auto& e : it->second) {
            if (e.supplycost < min_cost) min_cost = e.supplycost;
        }

        // Collect all suppliers with min cost
        for (auto& e : it->second) {
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

    // Sort: s_acctbal DESC, n_name ASC, s_name ASC, p_partkey ASC
    std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
        if (a.s_acctbal != b.s_acctbal) return a.s_acctbal > b.s_acctbal;
        if (a.n_name != b.n_name) return a.n_name < b.n_name;
        if (a.s_name != b.s_name) return a.s_name < b.s_name;
        return a.p_partkey < b.p_partkey;
    });

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
}
