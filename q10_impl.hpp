#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <string>
#include <algorithm>

inline void run_q10_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q10_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // o_orderdate >= '1993-08-01' AND o_orderdate < '1993-11-01'
    const Date date_lo = date_to_epoch(1993, 8, 1);
    const Date date_hi = date_to_epoch(1993, 11, 1);

    // Dense per-customer revenue accumulator (custkeys are dense 1..n_customer).
    // Group key in TPC-H Q10 is c_custkey (the other grouping columns are
    // functionally dependent on it), so a single int64 per customer suffices.
    std::vector<int64_t> cust_revenue(db->n_customer + 1, 0);

    // Drive from the (small) set of date-qualifying orders and gather only the
    // lineitems belonging to those orders via the orderkey CSR range, instead
    // of scanning the entire lineitem table.  This mirrors the plan's
    // orders→lineitem join with the orders side as the small build input.
    {
        PROFILE_SCOPE("q10_orders_lineitem_join_agg");
        const bool have_csr = db->lineitem_sorted_by_orderkey &&
                              !db->orderkey_lineitem_range.empty();
        for (int32_t oi = 0; oi < db->n_orders; oi++) {
            const Date od = db->o_orderdate[oi];
            if (od < date_lo || od >= date_hi) continue;

            const int32_t orderkey = db->o_orderkey[oi];
            const int32_t custkey = db->o_custkey[oi];

            int32_t start, end;
            if (have_csr) {
                const auto& r = db->orderkey_lineitem_range[orderkey];
                start = r.start;
                end = r.end;
            } else {
                continue;
            }

            int64_t rev = 0;
            for (int32_t j = start; j < end; j++) {
                TRACE_INC(li_scanned);
                if (db->l_returnflag[j] != 'R') continue;
                TRACE_INC(li_emitted);
                rev += db->l_extendedprice[j] * (100 - db->l_discount[j]); // scale 4
            }
            cust_revenue[custkey] += rev;
        }
    }
    TRACE_COUNT("q10_rows_scanned", li_scanned);
    TRACE_COUNT("q10_join_rows_emitted", li_emitted);
    TRACE_COUNT("q10_agg_rows_in", li_emitted);

    // Build result rows
    struct ResultRow {
        int32_t c_custkey;
        int64_t revenue;
    };
    std::vector<ResultRow> results;
    for (int32_t ck = 1; ck <= db->n_customer; ck++) {
        if (cust_revenue[ck] != 0) results.push_back({ck, cust_revenue[ck]});
    }
    TRACE_COUNT("q10_groups_created", (uint64_t)results.size());
    TRACE_COUNT("q10_agg_rows_emitted", (uint64_t)results.size());

    // Order by revenue desc
    TRACE_COUNT("q10_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q10_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q10_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q10_output");
    write_csv_header(out, {"c_custkey","c_name","revenue","c_acctbal","n_name","c_address","c_phone","c_comment"});

    // Fast manual CSV serialization into a single buffer.  Avoids per-field
    // std::string allocations and ostringstream overhead in the hot output
    // loop (which dominates this query's runtime).
    std::string buf;
    buf.reserve((size_t)results.size() * 160 + 64);

    char numbuf[32];
    auto append_int = [&](int64_t v) {
        if (v == 0) { buf.push_back('0'); return; }
        bool neg = v < 0;
        uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
        char* p = numbuf + sizeof(numbuf);
        while (u) { *--p = char('0' + (u % 10)); u /= 10; }
        if (neg) *--p = '-';
        buf.append(p, numbuf + sizeof(numbuf) - p);
    };
    auto append_money = [&](int64_t cents, int scale) {
        bool neg = cents < 0;
        uint64_t u = neg ? (uint64_t)(-cents) : (uint64_t)cents;
        char* p = numbuf + sizeof(numbuf);
        for (int i = 0; i < scale; ++i) { *--p = char('0' + (u % 10)); u /= 10; }
        *--p = '.';
        if (u == 0) { *--p = '0'; }
        else { while (u) { *--p = char('0' + (u % 10)); u /= 10; } }
        if (neg) *--p = '-';
        buf.append(p, numbuf + sizeof(numbuf) - p);
    };
    auto append_quoted = [&](const std::string& s) {
        static const bool* SPECIAL = []{
            static bool t[256] = {false};
            t[(unsigned char)','] = true;
            t[(unsigned char)'"'] = true;
            t[(unsigned char)'\n'] = true;
            t[(unsigned char)'\r'] = true;
            t[(unsigned char)'\\'] = true;
            return t;
        }();
        const char* d = s.data();
        const size_t n = s.size();
        bool needs = false;
        for (size_t k = 0; k < n; k++) {
            if (SPECIAL[(unsigned char)d[k]]) { needs = true; break; }
        }
        if (!needs) { buf.append(d, n); return; }
        buf.push_back('"');
        for (size_t k = 0; k < n; k++) {
            if (d[k] == '"') buf.push_back('"');
            buf.push_back(d[k]);
        }
        buf.push_back('"');
    };

    const size_t nres = results.size();
    const int PD_FAR = 48;
    const int PD_NEAR = 16;
    for (size_t i = 0; i < nres; i++) {
        if (i + PD_FAR < nres) {
            int32_t fidx = results[i + PD_FAR].c_custkey - 1;
            __builtin_prefetch(&db->c_name[fidx]);
            __builtin_prefetch(&db->c_address[fidx]);
            __builtin_prefetch(&db->c_phone[fidx]);
            __builtin_prefetch(&db->c_comment[fidx]);
        }
        if (i + PD_NEAR < nres) {
            int32_t nidx = results[i + PD_NEAR].c_custkey - 1;
            __builtin_prefetch(db->c_name[nidx].data());
            __builtin_prefetch(db->c_address[nidx].data());
            __builtin_prefetch(db->c_comment[nidx].data());
        }
        const ResultRow& r = results[i];
        int32_t c_idx = r.c_custkey - 1;
        int32_t nk = db->c_nationkey[c_idx];
        append_int(r.c_custkey);            buf.push_back(',');
        append_quoted(db->c_name[c_idx]);   buf.push_back(',');
        append_money(r.revenue, 4);         buf.push_back(',');
        append_money(db->c_acctbal[c_idx], 2); buf.push_back(',');
        append_quoted(db->n_name[nk]);      buf.push_back(',');
        append_quoted(db->c_address[c_idx]); buf.push_back(',');
        append_quoted(db->c_phone[c_idx]);  buf.push_back(',');
        append_quoted(db->c_comment[c_idx]);
        buf.push_back('\n');
    }
    out.write(buf.data(), (std::streamsize)buf.size());
    TRACE_COUNT("q10_query_output_rows", (uint64_t)results.size());
}
