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

    // Two-pass output to keep customer-column access cache-friendly:
    //  Pass 1 walks customers in custkey order (sequential over the column
    //  arrays) and serializes each qualifying row into a compact fragment
    //  buffer.  Pass 2 sorts tiny fixed-size records by revenue and stitches
    //  the pre-formatted fragments together.  This moves the random,
    //  cache-missing access off the scattered std::string heap data and onto a
    //  single dense fragment buffer.
    struct Rec {
        int64_t revenue;
        uint32_t off;
        uint32_t len;
    };
    std::vector<Rec> recs;
    std::vector<int32_t> qual;
    {
        for (int32_t ck = 1; ck <= db->n_customer; ck++)
            if (cust_revenue[ck] != 0) qual.push_back(ck);
        recs.reserve(qual.size());
    }

    std::string frag;
    frag.reserve(recs.capacity() * 160 + 64);

    char numbuf[32];
    auto append_int = [&](int64_t v) {
        if (v == 0) { frag.push_back('0'); return; }
        bool neg = v < 0;
        uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
        char* p = numbuf + sizeof(numbuf);
        while (u) { *--p = char('0' + (u % 10)); u /= 10; }
        if (neg) *--p = '-';
        frag.append(p, numbuf + sizeof(numbuf) - p);
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
        frag.append(p, numbuf + sizeof(numbuf) - p);
    };
    static const bool* SPECIAL = []{
        static bool t[256] = {false};
        t[(unsigned char)','] = true;
        t[(unsigned char)'"'] = true;
        t[(unsigned char)'\n'] = true;
        t[(unsigned char)'\r'] = true;
        t[(unsigned char)'\\'] = true;
        return t;
    }();
    auto append_quoted = [&](const std::string& s) {
        const char* d = s.data();
        const size_t n = s.size();
        bool needs = false;
        for (size_t k = 0; k < n; k++) {
            if (SPECIAL[(unsigned char)d[k]]) { needs = true; break; }
        }
        if (!needs) { frag.append(d, n); return; }
        frag.push_back('"');
        for (size_t k = 0; k < n; k++) {
            if (d[k] == '"') frag.push_back('"');
            frag.push_back(d[k]);
        }
        frag.push_back('"');
    };

    // Pass 1: serialize qualifying rows (custkey order) with two-level prefetch.
    const size_t nq = qual.size();
    const int P1_FAR = 32;
    const int P1_NEAR = 12;
    for (size_t qi = 0; qi < nq; qi++) {
        if (qi + P1_FAR < nq) {
            int32_t fi = qual[qi + P1_FAR] - 1;
            __builtin_prefetch(&db->c_name[fi]);
            __builtin_prefetch(&db->c_address[fi]);
            __builtin_prefetch(&db->c_comment[fi]);
        }
        if (qi + P1_NEAR < nq) {
            int32_t ni = qual[qi + P1_NEAR] - 1;
            __builtin_prefetch(db->c_name[ni].data());
            __builtin_prefetch(db->c_address[ni].data());
            __builtin_prefetch(db->c_comment[ni].data());
        }
        int32_t ck = qual[qi];
        int64_t rev = cust_revenue[ck];
        int32_t c_idx = ck - 1;
        int32_t nk = db->c_nationkey[c_idx];
        uint32_t off = (uint32_t)frag.size();
        append_int(ck);                       frag.push_back(',');
        append_quoted(db->c_name[c_idx]);     frag.push_back(',');
        append_money(rev, 4);                 frag.push_back(',');
        append_money(db->c_acctbal[c_idx], 2); frag.push_back(',');
        append_quoted(db->n_name[nk]);        frag.push_back(',');
        append_quoted(db->c_address[c_idx]);  frag.push_back(',');
        append_quoted(db->c_phone[c_idx]);    frag.push_back(',');
        append_quoted(db->c_comment[c_idx]);
        frag.push_back('\n');
        recs.push_back({rev, off, (uint32_t)(frag.size() - off)});
    }
    TRACE_COUNT("q10_groups_created", (uint64_t)recs.size());
    TRACE_COUNT("q10_agg_rows_emitted", (uint64_t)recs.size());

    // Order by revenue desc
    TRACE_COUNT("q10_sort_rows_in", (uint64_t)recs.size());
    {
        PROFILE_SCOPE("q10_sort");
        std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
            return a.revenue > b.revenue;
        });
    }
    TRACE_COUNT("q10_sort_rows_out", (uint64_t)recs.size());

    PROFILE_SCOPE("q10_output");
    write_csv_header(out, {"c_custkey","c_name","revenue","c_acctbal","n_name","c_address","c_phone","c_comment"});

    // Pass 2: stitch fragments in revenue order into the final buffer.
    std::string buf;
    buf.reserve(frag.size() + 64);
    const char* fbase = frag.data();
    const size_t nres = recs.size();
    const int PD = 24;
    for (size_t i = 0; i < nres; i++) {
        if (i + PD < nres) __builtin_prefetch(fbase + recs[i + PD].off);
        buf.append(fbase + recs[i].off, recs[i].len);
    }
    out.write(buf.data(), (std::streamsize)buf.size());
    TRACE_COUNT("q10_query_output_rows", (uint64_t)recs.size());
}
