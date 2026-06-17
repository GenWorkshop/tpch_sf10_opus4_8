#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_map>
#include <algorithm>

inline void run_q15_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q15_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // CTE revenue: l_shipdate >= '1996-02-01' AND l_shipdate < '1996-05-01'
    const Date date_lo = date_to_epoch(1996, 2, 1);
    const Date date_hi = date_to_epoch(1996, 5, 1);

    // Compute total_revenue per supplier using a dense array keyed by suppkey
    // (suppkey is dense 1..n_supplier, so this is a perfect-hash group-by).
    const int32_t n_supp = db->n_supplier;
    std::vector<int64_t> supp_revenue(n_supp + 1, 0); // index = suppkey

    {
        PROFILE_SCOPE("q15_lineitem_scan_agg");
        const Date* __restrict shipdate = db->l_shipdate.data();
        const int64_t* __restrict eprice = db->l_extendedprice.data();
        const int64_t* __restrict disc = db->l_discount.data();
        const int32_t* __restrict suppkey = db->l_suppkey.data();
        int64_t* __restrict acc = supp_revenue.data();
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            const Date d = shipdate[i];
            if (d >= date_lo && d < date_hi) {
                TRACE_INC(li_emitted);
                acc[suppkey[i]] += eprice[i] * (100 - disc[i]);
            }
        }
    }
    TRACE_COUNT("q15_rows_scanned", li_scanned);
    TRACE_COUNT("q15_rows_emitted", li_emitted);
    TRACE_COUNT("q15_agg_rows_in", li_emitted);
    TRACE_COUNT("q15_groups_created", (uint64_t)n_supp);

    // Find max revenue
    int64_t max_rev = 0;
    for (int32_t sk = 1; sk <= n_supp; sk++) {
        if (supp_revenue[sk] > max_rev) max_rev = supp_revenue[sk];
    }

    // Collect suppliers with max revenue (already in suppkey order)
    struct ResultRow {
        int32_t s_suppkey;
        int64_t total_revenue;
    };
    std::vector<ResultRow> results;
    for (int32_t sk = 1; sk <= n_supp; sk++) {
        if (supp_revenue[sk] == max_rev) {
            results.push_back({sk, supp_revenue[sk]});
        }
    }

    // Order by s_suppkey
    TRACE_COUNT("q15_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q15_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.s_suppkey < b.s_suppkey;
        });
    }
    TRACE_COUNT("q15_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q15_output");
    write_csv_header(out, {"s_suppkey","s_name","s_address","s_phone","total_revenue"});
    for (auto& r : results) {
        int32_t s_idx = r.s_suppkey - 1;
        write_csv_row(out, {
            std::to_string(r.s_suppkey),
            csv_quote(db->s_name[s_idx]),
            csv_quote(db->s_address[s_idx]),
            csv_quote(db->s_phone[s_idx]),
            fmt_money(r.total_revenue, 4)
        });
    }
    TRACE_COUNT("q15_query_output_rows", (uint64_t)results.size());
}
