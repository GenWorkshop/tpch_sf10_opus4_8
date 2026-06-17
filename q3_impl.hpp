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
    for (int32_t i = 0; i < db->n_customer; i++) {
        if (db->c_mktsegment[i] == "BUILDING") {
            cust_building[(size_t)i >> 6] |= (uint64_t)1 << (i & 63);
        }
    }
    }

    // Step 2: Qualifying orders (customer in BUILDING, orderdate < 1995-03-08).
    // Build a compact qualifies-bitmap indexed by orderkey (~max_orderkey/8
    // bytes, L3-resident) used to cheaply gate the lineitem scan before doing
    // any large random gathers. Branchless append avoids ~50% branch
    // mispredictions on the orderdate filter.
    std::vector<int32_t> qual_orders(db->n_orders);
    std::vector<uint64_t> qbits((size_t)(db->max_orderkey) / 64 + 1, 0);
    size_t qcount = 0;
    { PROFILE_SCOPE("q3_p2_orders");
    const Date* __restrict od = db->o_orderdate.data();
    const int32_t* __restrict ock = db->o_custkey.data();
    const int32_t* __restrict ook = db->o_orderkey.data();
    const uint64_t* __restrict cb = cust_building.data();
    int32_t* __restrict qo = qual_orders.data();
    uint64_t* __restrict qbp = qbits.data();
    const int32_t n = db->n_orders;
    for (int32_t i = 0; i < n; i++) {
        uint32_t c = (uint32_t)(ock[i] - 1);
        unsigned bld = (unsigned)((cb[c >> 6] >> (c & 63)) & 1);
        unsigned pass = (unsigned)(od[i] < date_filter) & bld;
        qo[qcount] = i;
        qcount += pass;
        if (pass) {
            uint32_t ok = (uint32_t)ook[i];
            qbp[ok >> 6] |= (uint64_t)1 << (ok & 63);
        }
    }
    }
    qual_orders.resize(qcount);

    // Dense revenue accumulator indexed by order row index (touched only on
    // the few matching lineitems, so its large size never hurts the hot loop).
    std::vector<int64_t> order_acc;
    { PROFILE_SCOPE("q3_p3_acc_alloc");
    order_acc.assign(db->n_orders, 0);
    }

    // Step 3: Scan lineitem; filter l_shipdate and gate on qualifies-bitmap.
    // Branchless: lineitem is clustered by orderkey, so the bitmap is touched
    // in streaming order (cache-friendly), letting us drop the ~50% shipdate
    // branch entirely.
    {
        PROFILE_SCOPE("q3_lineitem_scan_join_agg");
        const Date* __restrict l_shipdate = db->l_shipdate.data();
        const int32_t* __restrict l_orderkey = db->l_orderkey.data();
        const int64_t* __restrict l_extendedprice = db->l_extendedprice.data();
        const int64_t* __restrict l_discount = db->l_discount.data();
        const uint64_t* __restrict qb = qbits.data();
        const int32_t* __restrict ok2idx = db->orderkey_to_idx.data();
        int64_t* __restrict acc = order_acc.data();
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            uint32_t ok = (uint32_t)l_orderkey[i];
            unsigned pass = (unsigned)(l_shipdate[i] > date_filter);
            unsigned hit = pass & (unsigned)((qb[ok >> 6] >> (ok & 63)) & 1);
            if (hit) {
                TRACE_INC(li_emitted);
                int32_t o_idx = ok2idx[ok];
                acc[o_idx] += l_extendedprice[i] * (100 - l_discount[i]);
            }
        }
    }
    TRACE_COUNT("q3_rows_scanned", li_scanned);
    TRACE_COUNT("q3_join_rows_emitted", li_emitted);
    TRACE_COUNT("q3_agg_rows_in", li_emitted);

    // Step 4: Collect results from qualifying orders that matched a lineitem.
    struct ResultRow {
        int32_t orderkey;
        int64_t revenue;
        Date o_orderdate;
        int32_t o_shippriority;
    };
    std::vector<ResultRow> results;
    results.reserve(qual_orders.size());
    for (int32_t idx : qual_orders) {
        int64_t rev = order_acc[idx];
        if (rev > 0) {
            results.push_back({db->o_orderkey[idx], rev,
                               db->o_orderdate[idx], db->o_shippriority[idx]});
        }
    }
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
