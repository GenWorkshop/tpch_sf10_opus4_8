#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <map>
#include <cstdlib>

// Extract year from epoch days
inline int32_t epoch_to_year(int32_t days) {
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    return y;
}

inline void run_q7_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q7_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Find nationkeys for ALGERIA and BRAZIL
    int32_t nk_algeria = db->nation_name_to_key["ALGERIA"];
    int32_t nk_brazil = db->nation_name_to_key["BRAZIL"];

    // l_shipdate between '1995-01-01' and '1996-12-31'
    const Date date_lo = date_to_epoch(1995, 1, 1);
    const Date date_hi = date_to_epoch(1996, 12, 31);

    // Supplier nation code: 0=ALGERIA, 1=BRAZIL, -1=irrelevant (indexed by suppkey-1)
    std::vector<int8_t> s_code(db->n_supplier, -1);
    for (int32_t i = 0; i < db->n_supplier; i++) {
        int32_t sn = db->s_nationkey[i];
        if (sn == nk_algeria) s_code[i] = 0;
        else if (sn == nk_brazil) s_code[i] = 1;
    }

    // Aggregation: only 4 groups -> acc[supp_code][year-1995], revenue scale 4.
    int64_t acc[2][2] = {{0, 0}, {0, 0}};

    const int64_t n = db->n_lineitem;
    const Date* __restrict shipdate = db->l_shipdate.data();
    const int32_t* __restrict lsupp = db->l_suppkey.data();
    const int32_t* __restrict lorder = db->l_orderkey.data();
    const int64_t* __restrict lprice = db->l_extendedprice.data();
    const int64_t* __restrict ldisc = db->l_discount.data();
    const int8_t* __restrict scode = s_code.data();

    {
        PROFILE_SCOPE("q7_lineitem_scan_join_agg");
        if (db->lineitem_sorted_by_orderkey && db->orders_sorted_by_orderkey) {
            // Phase 1: branchless filter on (date range AND supplier nation),
            // streaming only shipdate/suppkey sequentially (2 streams). Survivors
            // (~2.4%) store just (lineitem_idx<<1)|supp_code. The orderkey is
            // fetched sparsely (with prefetch) in phase 2, avoiding a third 240MB
            // sequential stream here. Buffer is malloc'd (no zero-init).
            int32_t* __restrict buf = (int32_t*)std::malloc((size_t)(n + 1) * sizeof(int32_t));
            size_t np = 0;
            const uint32_t range = (uint32_t)(date_hi - date_lo);
            for (int64_t i = 0; i < n; i++) {
                int8_t sc = scode[lsupp[i] - 1];
                int cond = ((uint32_t)(shipdate[i] - date_lo) <= range) & (sc >= 0);
                buf[np] = (int32_t)((i << 1) | (sc & 1));
                np += (size_t)cond;
            }
            TRACE_ADD(li_scanned, n);

            // Phase 2: merge-join survivors against orders (both ascending by
            // orderkey) to resolve customer nation, then aggregate. Orderkey is
            // read sparsely from lorder[idx]; prefetch ahead to hide latency.
            const int32_t* __restrict o_ok = db->o_orderkey.data();
            const int32_t* __restrict o_ck = db->o_custkey.data();
            const int32_t* __restrict c_nk = db->c_nationkey.data();
            const int32_t n_orders = db->n_orders;
            int32_t op = 0;
            int32_t cur_ok = -1;
            int8_t cur_ccode = -1;
            const int PD = 32;
            for (size_t k = 0; k < np; k++) {
                if (k + PD < np) __builtin_prefetch(&lorder[buf[k + PD] >> 1], 0, 0);
                int32_t isc = buf[k];
                int64_t i = isc >> 1;
                int32_t ok = lorder[i];
                if (ok != cur_ok) {
                    while (op < n_orders && o_ok[op] < ok) op++;
                    cur_ok = ok;
                    int8_t cc = -1;
                    int32_t cn = c_nk[o_ck[op] - 1];
                    if (cn == nk_algeria) cc = 0;
                    else if (cn == nk_brazil) cc = 1;
                    cur_ccode = cc;
                }
                int8_t cc = cur_ccode;
                int8_t sc = (int8_t)(isc & 1);
                if (cc < 0 || cc == sc) continue;  // need opposite (ALG,BRA)/(BRA,ALG)
                TRACE_INC(li_emitted);
                int32_t y = epoch_to_year(shipdate[i]) - 1995;
                acc[sc][y] += lprice[i] * (100 - ldisc[i]);
            }
            std::free(buf);
        } else {
            // Fallback: random-access join via orderkey_to_idx.
            std::vector<int8_t> order_cust_nation(db->n_orders, -1);
            for (int32_t i = 0; i < db->n_orders; i++) {
                int32_t c_idx = db->o_custkey[i] - 1;
                if (c_idx >= 0 && c_idx < db->n_customer) {
                    int32_t cn = db->c_nationkey[c_idx];
                    if (cn == nk_algeria) order_cust_nation[i] = 0;
                    else if (cn == nk_brazil) order_cust_nation[i] = 1;
                }
            }
            for (int64_t i = 0; i < n; i++) {
                TRACE_INC(li_scanned);
                Date sd = shipdate[i];
                if (sd < date_lo || sd > date_hi) continue;
                int8_t sc = scode[lsupp[i] - 1];
                if (sc < 0) continue;
                int32_t ok = lorder[i];
                if (ok > db->max_orderkey) continue;
                int32_t o_idx = db->orderkey_to_idx[ok];
                if (o_idx < 0) continue;
                int8_t cc = order_cust_nation[o_idx];
                if (cc < 0 || cc == sc) continue;
                TRACE_INC(li_emitted);
                int32_t y = epoch_to_year(sd) - 1995;
                acc[sc][y] += lprice[i] * (100 - ldisc[i]);
            }
        }
    }
    TRACE_COUNT("q7_rows_scanned", li_scanned);
    TRACE_COUNT("q7_join_rows_emitted", li_emitted);
    TRACE_COUNT("q7_agg_rows_in", li_emitted);
    TRACE_COUNT("q7_groups_created", (uint64_t)4);
    TRACE_COUNT("q7_agg_rows_emitted", (uint64_t)4);

    PROFILE_SCOPE("q7_output");
    write_csv_header(out, {"supp_nation","cust_nation","l_year","revenue"});

    const char* names[] = {"ALGERIA", "BRAZIL"};
    // ORDER BY supp_nation, cust_nation, l_year.  supp=0(ALGERIA) sorts before
    // supp=1(BRAZIL); cust is the opposite nation so ordering by supp gives the
    // right group order, then year ascending.
    for (int sc = 0; sc < 2; sc++) {
        for (int y = 0; y < 2; y++) {
            if (acc[sc][y] == 0) continue;
            write_csv_row(out, {
                std::string(names[sc]),
                std::string(names[1 - sc]),
                std::to_string(1995 + y),
                fmt_money(acc[sc][y], 4)
            });
        }
    }
    TRACE_COUNT("q7_query_output_rows", (uint64_t)4);
}
