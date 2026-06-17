#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <algorithm>

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

    // o_orderstatus = 'F', indexed by orderkey.
    std::vector<uint8_t> order_is_F(maxok + 1, 0);
    for (int32_t i = 0; i < db->n_orders; i++) {
        if (db->o_orderstatus[i] == 'F') order_is_F[db->o_orderkey[i]] = 1;
    }

    // Per-order aggregates, indexed by orderkey.
    std::vector<int32_t> first_supp(maxok + 1, 0);
    std::vector<int32_t> late_supp(maxok + 1, 0);
    std::vector<uint8_t> flag(maxok + 1, 0);

    {
        PROFILE_SCOPE("q21_lineitem_scan_join");
        const int32_t* __restrict ok_col = db->l_orderkey.data();
        const int32_t* __restrict sk_col = db->l_suppkey.data();
        const Date* __restrict rcpt = db->l_receiptdate.data();
        const Date* __restrict cmit = db->l_commitdate.data();
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
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
    TRACE_COUNT("q21_rows_scanned", li_scanned);
    TRACE_COUNT("q21_join_rows_emitted", li_emitted);

    // numwait per supplier, dense indexed by suppkey (1-based).
    std::vector<int64_t> supp_numwait(db->n_supplier + 1, 0);
    {
        PROFILE_SCOPE("q21_order_agg");
        for (int32_t j = 0; j < db->n_orders; j++) {
            if (db->o_orderstatus[j] != 'F') continue;
            const int32_t ok = db->o_orderkey[j];
            const uint8_t f = flag[ok];
            if (!(f & 1)) continue;            // need another supplier (EXISTS)
            if (f & 2) continue;               // 2+ late suppliers kills it
            const int32_t ls = late_supp[ok];
            if (ls == 0) continue;             // no late supplier
            if (is_russia[ls]) supp_numwait[ls]++;
        }
    }

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
