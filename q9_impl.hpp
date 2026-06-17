#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp"  // date_to_epoch
#include "q7_impl.hpp"  // epoch_to_year
#include <ostream>
#include <vector>
#include <algorithm>
#include <cstdint>

inline void run_q9_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q9_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);

    const int32_t n_part = db->n_part;

    // ---- Filter part: p_name like '%rosy%'  → dense ids for matching parts ----
    // match[pk] (1-byte) is the hot per-lineitem filter; part_dense[pk] gives the
    // compact partsupp slot, only touched for survivors.
    std::vector<uint8_t> match(n_part + 1, 0);
    std::vector<int32_t> part_dense(n_part + 1, -1);
    int32_t nd = 0;
    for (int32_t i = 0; i < n_part; i++) {
        if (db->p_name[i].find("rosy") != std::string::npos) {
            match[i + 1] = 1;
            part_dense[i + 1] = nd++;
        }
    }

    // ---- Build per-(matching)part contiguous partsupp arrays ----
    // ps_off[d]..ps_off[d+1] index ps_sk[]/ps_cv[] for dense part d.
    std::vector<int32_t> ps_off(nd + 1, 0);
    for (int32_t i = 0; i < db->n_partsupp; i++) {
        int32_t pk = db->ps_partkey[i];
        if (pk <= n_part && match[pk]) ps_off[part_dense[pk] + 1]++;
    }
    for (int32_t d = 0; d < nd; d++) ps_off[d + 1] += ps_off[d];
    const int32_t ps_total = ps_off[nd];
    std::vector<int32_t> ps_sk(ps_total);
    std::vector<int64_t> ps_cv(ps_total);
    {
        std::vector<int32_t> fill(ps_off.begin(), ps_off.end());
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            int32_t pk = db->ps_partkey[i];
            if (pk <= n_part && match[pk]) {
                int32_t d = part_dense[pk];
                int32_t pos = fill[d]++;
                ps_sk[pos] = db->ps_suppkey[i];
                ps_cv[pos] = db->ps_supplycost[i];
            }
        }
    }

    // ---- Aggregate into dense (nation, year) array ----
    // years 1992..1998 → 7 buckets; nations 0..n_nations-1.
    const int32_t YEAR_BASE = 1992;
    const int32_t NYEARS = 7;
    const int32_t n_nations = db->n_nations;
    std::vector<__int128> acc((size_t)n_nations * NYEARS, 0);

    {
        PROFILE_SCOPE("q9_lineitem_scan_join_agg");
        const int32_t* L_part = db->l_partkey.data();
        const int32_t* L_supp = db->l_suppkey.data();
        const int32_t* L_ord  = db->l_orderkey.data();
        const int64_t* L_ext  = db->l_extendedprice.data();
        const int64_t* L_disc = db->l_discount.data();
        const int64_t* L_qty  = db->l_quantity.data();
        const int32_t* O2I    = db->orderkey_to_idx.data();
        const Date*    O_date = db->o_orderdate.data();
        const int32_t* S_nat  = db->s_nationkey.data();
        const uint8_t* M      = match.data();
        const int32_t  max_ok = db->max_orderkey;
        const int64_t  nli    = db->n_lineitem;

        for (int64_t i = 0; i < nli; i++) {
            TRACE_INC(li_scanned);
            int32_t partkey = L_part[i];
            if (!M[partkey]) continue;

            int32_t suppkey = L_supp[i];
            int32_t d = part_dense[partkey];
            // Find supplycost for this (part,supplier) in the small contiguous run.
            int64_t supplycost = -1;
            for (int32_t j = ps_off[d]; j < ps_off[d + 1]; j++) {
                if (ps_sk[j] == suppkey) { supplycost = ps_cv[j]; break; }
            }
            if (supplycost < 0) continue;

            int32_t orderkey = L_ord[i];
            if (orderkey > max_ok) continue;
            int32_t o_idx = O2I[orderkey];
            if (o_idx < 0) continue;
            int32_t year = epoch_to_year(O_date[o_idx]);

            int32_t nation_key = S_nat[suppkey - 1];

            TRACE_INC(li_emitted);
            __int128 amount = (__int128)L_ext[i] * (100 - L_disc[i])
                            - (__int128)supplycost * L_qty[i];

            int32_t yb = year - YEAR_BASE;
            acc[(size_t)nation_key * NYEARS + yb] += amount;
        }
    }
    TRACE_COUNT("q9_rows_scanned", li_scanned);
    TRACE_COUNT("q9_join_rows_emitted", li_emitted);
    TRACE_COUNT("q9_agg_rows_in", li_emitted);

    // ---- Build & sort results: nation name asc, year desc ----
    struct ResultRow {
        const std::string* nation;
        int32_t year;
        __int128 sum_profit;
    };
    std::vector<ResultRow> results;
    results.reserve((size_t)n_nations * NYEARS);
    for (int32_t nk = 0; nk < n_nations; nk++) {
        for (int32_t yb = 0; yb < NYEARS; yb++) {
            __int128 v = acc[(size_t)nk * NYEARS + yb];
            if (v != 0) results.push_back({&db->n_name[nk], YEAR_BASE + yb, v});
        }
    }
    TRACE_COUNT("q9_groups_created", (uint64_t)results.size());
    TRACE_COUNT("q9_agg_rows_emitted", (uint64_t)results.size());
    TRACE_COUNT("q9_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q9_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            int cmp = a.nation->compare(*b.nation);
            if (cmp != 0) return cmp < 0;
            return a.year > b.year;
        });
    }
    TRACE_COUNT("q9_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q9_output");
    write_csv_header(out, {"nation","o_year","sum_profit"});
    for (auto& r : results) {
        write_csv_row(out, {
            *r.nation,
            std::to_string(r.year),
            fmt_money(static_cast<long long>(r.sum_profit), 4)
        });
    }
    TRACE_COUNT("q9_query_output_rows", (uint64_t)results.size());
}
