#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

inline void run_q5_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q5_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // r_name = 'AFRICA' → regionkey
    int32_t target_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "AFRICA") { target_regionkey = i; break; }
    }

    // Nations in target region → small bool table indexed by nationkey (0..24)
    bool target_nation[64] = {false};
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == target_regionkey) {
            target_nation[i] = true;
        }
    }

    // Date filter: o_orderdate >= '1997-01-01' AND < '1998-01-01'
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    // Per-nation revenue accumulators (scale 4), indexed by nationkey.
    int64_t nation_revenue[64] = {0};

    const int32_t* __restrict o_orderdate = db->o_orderdate.data();
    const int32_t* __restrict o_custkey   = db->o_custkey.data();
    const int32_t* __restrict o_orderkey  = db->o_orderkey.data();
    const int32_t* __restrict c_nationkey = db->c_nationkey.data();
    const int32_t* __restrict s_nationkey = db->s_nationkey.data();
    const int32_t* __restrict l_suppkey   = db->l_suppkey.data();
    const int64_t* __restrict l_extprice  = db->l_extendedprice.data();
    const int64_t* __restrict l_discount  = db->l_discount.data();
    const auto* __restrict ranges = db->orderkey_lineitem_range.data();

    // Join order follows the DuckDB plan bottom-up: the highly selective
    // orders(date) ⋈ customer(nation∈AFRICA) result drives a CSR lookup into
    // lineitem, so we only touch lineitems of qualifying orders instead of
    // scanning all ~60M rows.
    {
        PROFILE_SCOPE("q5_lineitem_scan_join_agg");
        for (int32_t i = 0; i < db->n_orders; i++) {
            Date od = o_orderdate[i];
            if (od < date_lo || od >= date_hi) continue;
            int32_t c_idx = o_custkey[i] - 1;
            int32_t c_nation = c_nationkey[c_idx];
            if (!target_nation[c_nation]) continue;

            int32_t ok = o_orderkey[i];
            auto rng = ranges[ok];
            int64_t rev = 0;
            for (int32_t li = rng.start; li < rng.end; li++) {
                if (s_nationkey[l_suppkey[li] - 1] == c_nation) {
                    rev += l_extprice[li] * (100 - l_discount[li]); // scale 4
                    TRACE_INC(li_emitted);
                }
            }
            nation_revenue[c_nation] += rev;
        }
    }
    TRACE_COUNT("q5_rows_scanned", li_scanned);
    TRACE_COUNT("q5_join_rows_emitted", li_emitted);
    TRACE_COUNT("q5_agg_rows_in", li_emitted);
    TRACE_COUNT("q5_groups_created", (uint64_t)nation_revenue.size());
    TRACE_COUNT("q5_agg_rows_emitted", (uint64_t)nation_revenue.size());

    // Sort by revenue desc
    struct ResultRow {
        std::string n_name;
        int64_t revenue;
    };
    std::vector<ResultRow> results;
    for (int nk = 0; nk < db->n_nations; nk++) {
        if (nation_revenue[nk] != 0) {
            results.push_back({db->n_name[nk], nation_revenue[nk]});
        }
    }
    TRACE_COUNT("q5_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q5_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q5_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q5_output");
    write_csv_header(out, {"n_name","revenue"});
    for (auto& r : results) {
        write_csv_row(out, {r.n_name, fmt_money(r.revenue, 4)});
    }
    TRACE_COUNT("q5_query_output_rows", (uint64_t)results.size());
}
