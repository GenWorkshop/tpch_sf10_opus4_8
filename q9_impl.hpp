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
    // mbits is a bit-packed hot per-lineitem filter (≈250KB, L2-resident);
    // part_dense[pk] gives the compact partsupp slot, only touched for survivors.
    std::vector<uint64_t> mbits((n_part >> 6) + 2, 0);
    std::vector<int32_t> part_dense(n_part + 1, -1);
    int32_t nd = 0;
    for (int32_t i = 0; i < n_part; i++) {
        if (db->p_name[i].find("rosy") != std::string::npos) {
            int32_t pk = i + 1;
            mbits[pk >> 6] |= (uint64_t)1 << (pk & 63);
            part_dense[pk] = nd++;
        }
    }
    auto test_bit = [&](int32_t pk) -> bool {
        return (mbits[pk >> 6] >> (pk & 63)) & 1;
    };

    // ---- Build per-(matching)part contiguous partsupp arrays ----
    // ps_off[d]..ps_off[d+1] index ps_sk[]/ps_cv[] for dense part d.
    std::vector<int32_t> ps_off(nd + 1, 0);
    for (int32_t i = 0; i < db->n_partsupp; i++) {
        int32_t pk = db->ps_partkey[i];
        if (pk <= n_part && test_bit(pk)) ps_off[part_dense[pk] + 1]++;
    }
    for (int32_t d = 0; d < nd; d++) ps_off[d + 1] += ps_off[d];
    const int32_t ps_total = ps_off[nd];
    std::vector<int32_t> ps_sk(ps_total);
    std::vector<int64_t> ps_cv(ps_total);
    {
        std::vector<int32_t> fill(ps_off.begin(), ps_off.end());
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            int32_t pk = db->ps_partkey[i];
            if (pk <= n_part && test_bit(pk)) {
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
        const uint64_t* MB    = mbits.data();
        const int32_t  max_ok = db->max_orderkey;
        const int64_t  nli    = db->n_lineitem;

        // Phase A1: branchless left-pack part-filter over all lineitem rows.
        // No per-row data-dependent branch → the (random) bitmap probes for
        // consecutive rows issue concurrently (high memory-level parallelism).
        // Also carry partkey (already loaded) so A2 need not re-read L_part.
        std::vector<int32_t> sv_idx(nli / 12 + 16);
        std::vector<int32_t> sv_pk(nli / 12 + 16);
        int64_t ns = 0;
        {
        PROFILE_SCOPE("q9_phaseA_scan_filter");
        int32_t* SI = sv_idx.data();
        int32_t* SP = sv_pk.data();
        int64_t cap = (int64_t)sv_idx.size();
        for (int64_t i = 0; i < nli; i++) {
            uint32_t pk = (uint32_t)L_part[i];
            uint64_t bit = (MB[pk >> 6] >> (pk & 63)) & 1;
            if (ns >= cap - 1) {
                sv_idx.resize(cap * 2); sv_pk.resize(cap * 2);
                SI = sv_idx.data(); SP = sv_pk.data(); cap = (int64_t)sv_idx.size();
            }
            SI[ns] = (int32_t)i;
            SP[ns] = (int32_t)pk;
            ns += (int64_t)bit;
        }
        sv_idx.resize(ns); sv_pk.resize(ns);
        }
        TRACE_ADD(li_scanned, (uint64_t)nli);

        // Phase A2: process survivors (sequential-order indices → streaming
        // column reads). Cheap small-array joins: partsupp run + supplier nation.
        const int32_t* SI = sv_idx.data();
        const int32_t* SP = sv_pk.data();
        std::vector<int32_t> sv_ord(ns);
        std::vector<int8_t>  sv_nat(ns);
        std::vector<int64_t> sv_amt(ns);
        int64_t nv = 0;
        {
        PROFILE_SCOPE("q9_phaseA2_survivor_join");
        const int64_t PFD = 32;
        for (int64_t k = 0; k < ns; k++) {
            if (k + PFD < ns) __builtin_prefetch(&part_dense[SP[k + PFD]], 0, 0);
            int64_t i = SI[k];
            int32_t partkey = SP[k];
            int32_t suppkey = L_supp[i];
            int32_t d = part_dense[partkey];
            int64_t supplycost = -1;
            for (int32_t j = ps_off[d]; j < ps_off[d + 1]; j++) {
                if (ps_sk[j] == suppkey) { supplycost = ps_cv[j]; break; }
            }
            if (supplycost < 0) continue;
            int32_t orderkey = L_ord[i];
            if (orderkey > max_ok) continue;
            int64_t amount = L_ext[i] * (100 - L_disc[i]) - supplycost * L_qty[i];
            sv_ord[nv] = orderkey;
            sv_nat[nv] = (int8_t)S_nat[suppkey - 1];
            sv_amt[nv] = amount;
            nv++;
        }
        }

        const int32_t* SO = sv_ord.data();

        // Phase B: orderkey → order row index (random gather into ~240MB array),
        // prefetched.
        std::vector<int32_t> sv_oidx(nv);
        const int64_t PFB = 48;
        {
        PROFILE_SCOPE("q9_phaseB_orderkey_gather");
        for (int64_t k = 0; k < nv; k++) {
            if (k + PFB < nv) __builtin_prefetch(&O2I[SO[k + PFB]], 0, 0);
            int32_t ok = SO[k];
            sv_oidx[k] = (ok <= max_ok) ? O2I[ok] : -1;
        }
        }

        // Phase C: order row → orderdate (random gather into ~60MB array),
        // prefetched, then accumulate.
        const int32_t* OI = sv_oidx.data();
        const int8_t*  SN = sv_nat.data();
        const int64_t* SA = sv_amt.data();
        {
        PROFILE_SCOPE("q9_phaseC_date_agg");
        for (int64_t k = 0; k < nv; k++) {
            if (k + PFB < nv) {
                int32_t pidx = OI[k + PFB];
                if (pidx >= 0) __builtin_prefetch(&O_date[pidx], 0, 0);
            }
            int32_t o_idx = OI[k];
            if (o_idx < 0) continue;
            TRACE_INC(li_emitted);
            int32_t year = epoch_to_year(O_date[o_idx]);
            int32_t yb = year - YEAR_BASE;
            acc[(size_t)SN[k] * NYEARS + yb] += SA[k];
        }
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
