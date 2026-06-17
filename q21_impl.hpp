#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <algorithm>
#include <climits>
#include <immintrin.h>

// TPC-H Q21.  RUSSIA suppliers that are the SOLE late supplier on a multi-
// supplier 'F' order.  The DuckDB plan groups lineitem by (l_orderkey) and
// resolves the EXISTS / NOT-EXISTS correlated subqueries as per-order
// aggregates.  We mirror that with dense arrays indexed by orderkey instead of
// a hash map, so the 60M-row lineitem scan does no hashing or per-order
// allocation.
//
// Per order we track:
//   first_supp : first supplier seen        (0 = unseen)
//   late_supp  : first late supplier seen   (0 = none late)
//   flag bit0  : >=2 distinct suppliers      (the EXISTS other-supplier test)
//   flag bit1  : >=2 distinct late suppliers (kills the NOT-EXISTS test)
// An order contributes to supplier `ls = late_supp` when it is 'F', has a sole
// late supplier (bit1 clear, ls != 0), has another supplier (bit0 set) and ls
// is a RUSSIA supplier.
__attribute__((target("avx2")))
static inline void q21_scan_sorted(
    const int32_t* __restrict ok_col, const int32_t* __restrict sk_col,
    const Date* __restrict rcpt, const Date* __restrict cmit, int64_t n,
    const char* __restrict o_stat,
    int32_t n_ord, const uint8_t* __restrict is_russia,
    int64_t* __restrict supp_numwait) {
    // orders and lineitem are both clustered by orderkey and every order has
    // at least one lineitem (TPC-H), so the k-th lineitem run is exactly the
    // k-th order: o_orderstatus is read sequentially by run index, and the
    // o_orderkey column need not be touched at all.
    int32_t oj = 0;
    int64_t i = 0;
    while (i < n) {
        const int32_t ok = ok_col[i];
        // Locate the end of this orderkey run with an AVX2 scan: most runs
        // (TPC-H: 1-7 lines) terminate inside the first 8-lane compare, so
        // boundary detection costs ~1 vector op instead of a mispredicting
        // per-row branch.
        int64_t e = i;
        const __m256i vok = _mm256_set1_epi32(ok);
        bool done = false;
        while (e + 8 <= n) {
            __m256i v = _mm256_loadu_si256(
                reinterpret_cast<const __m256i*>(ok_col + e));
            unsigned m = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(
                _mm256_cmpeq_epi32(v, vok)));
            if (m != 0xFFu) { e += __builtin_ctz(~m); done = true; break; }
            e += 8;
        }
        if (!done) while (e < n && ok_col[e] == ok) e++;

        if (o_stat[oj] == 'F') {
            int32_t min_sk = INT32_MAX, max_sk = 0;
            int32_t late_min = INT32_MAX, late_max = 0;
            for (int64_t j = i; j < e; j++) {    // counted loop, no boundary branch
                const int32_t sk = sk_col[j];
                min_sk = std::min(min_sk, sk);
                max_sk = std::max(max_sk, sk);
                const bool late = rcpt[j] > cmit[j];
                late_min = std::min(late_min, late ? sk : INT32_MAX);
                late_max = std::max(late_max, late ? sk : 0);
            }
            if (max_sk != min_sk &&          // >=2 distinct suppliers (EXISTS)
                late_max != 0 &&             // at least one late supplier
                late_max == late_min &&      // exactly one late supplier
                is_russia[late_min])
                supp_numwait[late_min]++;
        }
        i = e;
        oj++;
    }
}

inline void run_q21_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q21_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);

    const int32_t russia_nk = db->nation_name_to_key["RUSSIA"];

    // RUSSIA suppliers: dense bool indexed by suppkey (1-based).
    std::vector<uint8_t> is_russia(db->n_supplier + 1, 0);
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (db->s_nationkey[i] == russia_nk) is_russia[i + 1] = 1;
    }

    const int32_t maxok = db->max_orderkey;

    // numwait per supplier, dense indexed by suppkey (1-based).
    std::vector<int64_t> supp_numwait(db->n_supplier + 1, 0);

    const int32_t* __restrict ok_col = db->l_orderkey.data();
    const int32_t* __restrict sk_col = db->l_suppkey.data();
    const Date* __restrict rcpt = db->l_receiptdate.data();
    const Date* __restrict cmit = db->l_commitdate.data();
    const int64_t n = db->n_lineitem;

    if (db->lineitem_sorted_by_orderkey && db->orders_sorted_by_orderkey) {
        // Both tables clustered by orderkey: merge-walk them in lockstep, using
        // an AVX2 run-boundary scan and a branchless min/max per-order
        // reduction (multi == max_sk!=min_sk; sole late supplier == late_min).
        PROFILE_SCOPE("q21_lineitem_scan_join");
        q21_scan_sorted(ok_col, sk_col, rcpt, cmit, n,
                        db->o_orderstatus.data(),
                        db->n_orders, is_russia.data(), supp_numwait.data());
        TRACE_COUNT("q21_rows_scanned", li_scanned);
        TRACE_COUNT("q21_join_rows_emitted", li_emitted);
        goto finalize;
    }

    {
    // o_orderstatus = 'F', indexed by orderkey.
    std::vector<uint8_t> order_is_F(maxok + 1, 0);
    {
        PROFILE_SCOPE("q21_build_order_is_F");
        for (int32_t i = 0; i < db->n_orders; i++) {
            if (db->o_orderstatus[i] == 'F') order_is_F[db->o_orderkey[i]] = 1;
        }
    }

    if (db->lineitem_sorted_by_orderkey) {
        // Lineitem clustered by orderkey but orders not: run-walk lineitem,
        // reading F-status from the orderkey-indexed bitmap.
        PROFILE_SCOPE("q21_lineitem_scan_join");
        int64_t i = 0;
        while (i < n) {
            const int32_t ok = ok_col[i];
            if (!order_is_F[ok]) {                 // skip non-'F' orders cheaply
                do { i++; } while (i < n && ok_col[i] == ok);
                continue;
            }
            int32_t min_sk = INT32_MAX, max_sk = 0;
            int32_t late_min = INT32_MAX, late_max = 0;
            do {
                const int32_t sk = sk_col[i];
                min_sk = std::min(min_sk, sk);
                max_sk = std::max(max_sk, sk);
                const bool late = rcpt[i] > cmit[i];
                late_min = std::min(late_min, late ? sk : INT32_MAX);
                late_max = std::max(late_max, late ? sk : 0);
                i++;
            } while (i < n && ok_col[i] == ok);

            if (max_sk != min_sk &&
                late_max != 0 &&
                late_max == late_min &&
                is_russia[late_min])
                supp_numwait[late_min]++;
        }
    } else {
        // Fallback: dense per-order arrays indexed by orderkey.
        std::vector<int32_t> first_supp(maxok + 1, 0);
        std::vector<int32_t> late_supp(maxok + 1, 0);
        std::vector<uint8_t> flag(maxok + 1, 0);
        {
            PROFILE_SCOPE("q21_lineitem_scan_join");
            for (int64_t i = 0; i < n; i++) {
                const int32_t ok = ok_col[i];
                if (ok > maxok || !order_is_F[ok]) continue;
                TRACE_INC(li_emitted);
                const int32_t sk = sk_col[i];
                const int32_t fs = first_supp[ok];
                if (fs == 0) first_supp[ok] = sk;
                else if (sk != fs) flag[ok] |= 1;
                if (rcpt[i] > cmit[i]) {
                    const int32_t ls = late_supp[ok];
                    if (ls == 0) late_supp[ok] = sk;
                    else if (sk != ls) flag[ok] |= 2;
                }
            }
        }
        PROFILE_SCOPE("q21_order_agg");
        for (int32_t j = 0; j < db->n_orders; j++) {
            if (db->o_orderstatus[j] != 'F') continue;
            const int32_t ok = db->o_orderkey[j];
            const uint8_t f = flag[ok];
            if (!(f & 1)) continue;
            if (f & 2) continue;
            const int32_t ls = late_supp[ok];
            if (ls == 0) continue;
            if (is_russia[ls]) supp_numwait[ls]++;
        }
    }
    TRACE_COUNT("q21_rows_scanned", li_scanned);
    TRACE_COUNT("q21_join_rows_emitted", li_emitted);
    }

finalize: ;

    struct ResultRow {
        const std::string* s_name;
        int64_t numwait;
    };
    std::vector<ResultRow> results;
    for (int32_t sk = 1; sk <= db->n_supplier; sk++) {
        if (supp_numwait[sk]) results.push_back({&db->s_name[sk - 1], supp_numwait[sk]});
    }
    TRACE_COUNT("q21_agg_rows_emitted", (uint64_t)results.size());

    {
        PROFILE_SCOPE("q21_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.numwait != b.numwait) return a.numwait > b.numwait;
            return *a.s_name < *b.s_name;
        });
    }

    PROFILE_SCOPE("q21_output");
    write_csv_header(out, {"s_name", "numwait"});
    for (auto& r : results) {
        write_csv_row(out, {csv_quote(*r.s_name), std::to_string(r.numwait)});
    }
    TRACE_COUNT("q21_query_output_rows", (uint64_t)results.size());
}
