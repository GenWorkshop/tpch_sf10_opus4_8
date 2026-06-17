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

// q20: Potential part promotion - suppliers in FRANCE with partsupp where
// part name LIKE 'linen%' and ps_availqty > 0.5 * sum(l_quantity) for that
// (partkey, suppkey) pair in shipdate [1997-01-01, 1998-01-01)
// Order by s_name

static void run_q20(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1997, 1, 1);
    const int32_t date_end = to_days(1998, 1, 1);

    // Find FRANCE nationkey
    int32_t france_nk = -1;
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        if (db->n_name[i] == "FRANCE") { france_nk = db->n_nationkey[i]; break; }
    }

    // Find parts with p_name LIKE 'linen%'
    std::unordered_set<int32_t> linen_parts;
    for (int32_t pk = 1; pk < (int32_t)db->p_name.size(); pk++) {
        if (db->p_name[pk].substr(0, 5) == "linen") {
            linen_parts.insert(pk);
        }
    }

    // Compute sum(l_quantity) per (partkey, suppkey) for qualifying date range and parts
    // Key: partkey * 1000000 + suppkey
    std::unordered_map<int64_t, int64_t> ps_qty_sum;
    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q20_lineitem_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t sd = db->l_shipdate[i];
            if (sd >= date_start && sd < date_end) {
                int32_t pk = db->l_partkey[i];
                if (linen_parts.count(pk)) {
                    TRACE_INC(rows_emitted);
                    int64_t key = (int64_t)pk * 1000000LL + db->l_suppkey[i];
                    ps_qty_sum[key] += db->l_quantity[i];
                }
            }
        }
    }
    TRACE_COUNT("q20_linen_parts", linen_parts.size());
    TRACE_COUNT("q20_rows_scanned", rows_scanned);
    TRACE_COUNT("q20_rows_emitted", rows_emitted);
    TRACE_COUNT("q20_groups_created", ps_qty_sum.size());

    // Find qualifying suppkeys from partsupp
    // ps_availqty > 0.5 * sum(l_quantity) for that (partkey, suppkey)
    std::unordered_set<int32_t> qualifying_supps;
    TRACE_DECL(ps_scanned);
    {
        PROFILE_SCOPE("q20_partsupp_probe");
        for (int64_t i = 0; i < db->partsupp_count; i++) {
            TRACE_INC(ps_scanned);
            int32_t pk = db->ps_partkey[i];
            if (!linen_parts.count(pk)) continue;
            int32_t sk = db->ps_suppkey[i];
            int64_t key = (int64_t)pk * 1000000LL + sk;
            auto it = ps_qty_sum.find(key);
            // ps_availqty is integer, sum is scale 2
            // Condition: ps_availqty > 0.5 * sum(l_quantity)
            // ps_availqty (raw int) vs 0.5 * sum (scale 2)
            // Convert: ps_availqty * 100 (to scale 2) > 0.5 * sum
            // Or: ps_availqty * 200 > sum
            // If no matching lineitems, the SQL subquery returns NULL -> comparison is FALSE
            if (it == ps_qty_sum.end()) continue;
            int64_t avail_scaled = (int64_t)db->ps_availqty[i] * 200; // scale 2 * 2
            int64_t sum_qty = it->second;
            if (avail_scaled > sum_qty) {
                qualifying_supps.insert(sk);
            }
        }
    }
    TRACE_COUNT("q20_partsupp_scanned", ps_scanned);
    TRACE_COUNT("q20_qualifying_supps", qualifying_supps.size());

    // Filter suppliers in FRANCE
    struct Result { std::string name; std::string address; };
    std::vector<Result> results;
    for (int32_t sk : db->nation_to_suppliers[france_nk]) {
        if (qualifying_supps.count(sk)) {
            results.push_back({db->s_name[sk], db->s_address[sk]});
        }
    }

    {
        PROFILE_SCOPE("q20_sort");
        // Sort by s_name
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            return a.name < b.name;
        });
    }
    TRACE_COUNT("q20_sort_rows_in", results.size());
    TRACE_COUNT("q20_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q20_output");
    std::ostringstream oss;
    write_csv_header(oss, {"s_name","s_address"});
    for (auto& r : results) {
        write_csv_row(oss, {csv_quote(r.name), csv_quote(r.address)});
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q20_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
