#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // for date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

// Format epoch days as YYYY-MM-DD
inline std::string format_date(int32_t days) {
    // Civil date from days since 1970-01-01 (Howard Hinnant algorithm)
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned d = doy - (153*mp + 2)/5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    char buf[11];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", y, m, d);
    return std::string(buf);
}

inline void run_q3_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q3_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Filters:
    // c_mktsegment = 'BUILDING'
    // o_orderdate < '1995-03-08'
    // l_shipdate > '1995-03-08'
    const Date date_filter = date_to_epoch(1995, 3, 8);

    // Step 1: Find customers in BUILDING segment. Store as a bitmap (~n/8
    // bytes) so the per-order random gather in step 2 stays L2-resident.
    std::vector<uint64_t> cust_building((size_t)db->n_customer / 64 + 1, 0);
    { PROFILE_SCOPE("q3_p1_customer");
    const std::string* __restrict ms = db->c_mktsegment.data();
    const int32_t nc = db->n_customer;
    for (int32_t i = 0; i < nc; i++) {
        const std::string& s = ms[i];
        // "BUILDING" is the only segment of length 8 starting with 'B'.
        if (s.size() == 8 && s[0] == 'B') {
            cust_building[(size_t)i >> 6] |= (uint64_t)1 << (i & 63);
        }
    }
    }

    // Step 2: Qualifying orders (customer in BUILDING, orderdate < 1995-03-08).
    // Build a compact qualifies-bitmap indexed by orderkey (~max_orderkey/8
    // bytes, L3-resident) used to cheaply gate the lineitem scan before doing
    // any large random gathers.
    std::vector<uint64_t> qbits((size_t)(db->max_orderkey) / 64 + 1, 0);
    size_t qcount = 0;
    { PROFILE_SCOPE("q3_p2_orders");
    const Date* __restrict od = db->o_orderdate.data();
    const int32_t* __restrict ock = db->o_custkey.data();
    const int32_t* __restrict ook = db->o_orderkey.data();
    const uint64_t* __restrict cb = cust_building.data();
    uint64_t* __restrict qbp = qbits.data();
    const int32_t n = db->n_orders;
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)(ock[i] - 1);
        unsigned bld = (unsigned)((cb[c >> 6] >> (c & 63)) & 1);
        unsigned pass = (unsigned)(od[i] < date_filter) & bld;
        if (pass) {
            qcount++;
            uint32_t ok = (uint32_t)ook[i];
            qbp[ok >> 6] |= (uint64_t)1 << (ok & 63);
        }
    }
    }

    struct ResultRow {
        int32_t orderkey;
        int64_t revenue;
        Date o_orderdate;
        int32_t o_shippriority;
    };
    std::vector<ResultRow> results;
    results.reserve(qcount / 8 + 16);

    // Step 3: Scan lineitem; filter l_shipdate and gate on qualifies-bitmap.
    // Branchless: lineitem is clustered by orderkey, so the bitmap is touched
    // in streaming order (cache-friendly), letting us drop the ~50% shipdate
    // branch entirely. Because lineitem rows for an order are contiguous, we
    // aggregate revenue per orderkey with a run-length pass — no per-hit random
    // accumulator gather. orderdate/shippriority are looked up once per group.
    {
        PROFILE_SCOPE("q3_lineitem_scan_join_agg");
        const Date* __restrict l_shipdate = db->l_shipdate.data();
        const int32_t* __restrict l_orderkey = db->l_orderkey.data();
        const int64_t* __restrict l_extendedprice = db->l_extendedprice.data();
        const int64_t* __restrict l_discount = db->l_discount.data();
        const uint64_t* __restrict qb = qbits.data();
        const int32_t* __restrict ok2idx = db->orderkey_to_idx.data();
        const Date* __restrict o_orderdate = db->o_orderdate.data();
        const int32_t* __restrict o_shippriority = db->o_shippriority.data();
        const int64_t n = db->n_lineitem;
        int32_t cur_ok = -1;
        int64_t cur_rev = 0;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            uint32_t ok = (uint32_t)l_orderkey[i];
            unsigned pass = (unsigned)(l_shipdate[i] > date_filter);
            unsigned hit = pass & (unsigned)((qb[ok >> 6] >> (ok & 63)) & 1);
            if (hit) {
                TRACE_INC(li_emitted);
                int64_t rev = l_extendedprice[i] * (100 - l_discount[i]);
                if ((int32_t)ok == cur_ok) {
                    cur_rev += rev;
                } else {
                    if (cur_ok >= 0) {
                        int32_t o_idx = ok2idx[cur_ok];
                        results.push_back({cur_ok, cur_rev,
                                           o_orderdate[o_idx], o_shippriority[o_idx]});
                    }
                    cur_ok = (int32_t)ok;
                    cur_rev = rev;
                }
            }
        }
        if (cur_ok >= 0) {
            int32_t o_idx = ok2idx[cur_ok];
            results.push_back({cur_ok, cur_rev,
                               o_orderdate[o_idx], o_shippriority[o_idx]});
        }
    }
    TRACE_COUNT("q3_rows_scanned", li_scanned);
    TRACE_COUNT("q3_join_rows_emitted", li_emitted);
    TRACE_COUNT("q3_agg_rows_in", li_emitted);
    TRACE_COUNT("q3_groups_created", (uint64_t)results.size());
    TRACE_COUNT("q3_agg_rows_emitted", (uint64_t)results.size());
    TRACE_COUNT("q3_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q3_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            if (a.revenue != b.revenue) return a.revenue > b.revenue;
            return a.o_orderdate < b.o_orderdate;
        });
    }
    TRACE_COUNT("q3_sort_rows_out", (uint64_t)results.size());

    // Output
    PROFILE_SCOPE("q3_output");
    std::string buf;
    buf.reserve(results.size() * 40 + 64);
    buf += "l_orderkey,revenue,o_orderdate,o_shippriority\n";
    char tmp[32];
    for (auto& r : results) {
        // l_orderkey
        {
            char* p = tmp + sizeof(tmp);
            int32_t v = r.orderkey;
            do { *--p = char('0' + v % 10); v /= 10; } while (v);
            buf.append(p, tmp + sizeof(tmp) - p);
        }
        buf.push_back(',');
        // revenue (fixed-point, scale 4)
        {
            int64_t v = r.revenue; // always > 0 here
            int64_t whole = v / 10000;
            int frac = (int)(v % 10000);
            char* p = tmp + sizeof(tmp);
            *--p = char('0' + frac % 10); frac /= 10;
            *--p = char('0' + frac % 10); frac /= 10;
            *--p = char('0' + frac % 10); frac /= 10;
            *--p = char('0' + frac);
            *--p = '.';
            do { *--p = char('0' + whole % 10); whole /= 10; } while (whole);
            buf.append(p, tmp + sizeof(tmp) - p);
        }
        buf.push_back(',');
        // o_orderdate (YYYY-MM-DD)
        {
            int32_t days = r.o_orderdate;
            int32_t z = days + 719468;
            int32_t era = (z >= 0 ? z : z - 146096) / 146097;
            unsigned doe = static_cast<unsigned>(z - era * 146097);
            unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
            int y = static_cast<int>(yoe) + era * 400;
            unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
            unsigned mp = (5*doy + 2) / 153;
            unsigned d = doy - (153*mp + 2)/5 + 1;
            unsigned m = mp + (mp < 10 ? 3 : -9);
            y += (m <= 2);
            char* p = tmp;
            *p++ = char('0' + (y / 1000) % 10);
            *p++ = char('0' + (y / 100) % 10);
            *p++ = char('0' + (y / 10) % 10);
            *p++ = char('0' + y % 10);
            *p++ = '-';
            *p++ = char('0' + m / 10);
            *p++ = char('0' + m % 10);
            *p++ = '-';
            *p++ = char('0' + d / 10);
            *p++ = char('0' + d % 10);
            buf.append(tmp, p - tmp);
        }
        buf.push_back(',');
        // o_shippriority
        {
            char* p = tmp + sizeof(tmp);
            int32_t v = r.o_shippriority;
            do { *--p = char('0' + v % 10); v /= 10; } while (v);
            buf.append(p, tmp + sizeof(tmp) - p);
        }
        buf.push_back('\n');
    }
    out.write(buf.data(), buf.size());
    TRACE_COUNT("q3_query_output_rows", (uint64_t)results.size());
}
