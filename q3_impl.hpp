#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // for date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

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

    // Step 3: Scan lineitem. Decoupled, Neumann/Jasny-style: a fully
    // vectorizable SELECT phase (shipdate filter + bitmap probe, no
    // loop-carried dependency) collects the rare hit positions, then a cheap
    // scalar phase reads the wide money columns only for those hits and
    // run-length aggregates per (contiguous) orderkey.
    std::vector<int32_t> hits;
    hits.reserve((size_t)db->n_lineitem / 16 + 1024);
    {
        PROFILE_SCOPE("q3_lineitem_scan_join_agg");
        const Date* __restrict l_shipdate = db->l_shipdate.data();
        const int32_t* __restrict l_orderkey = db->l_orderkey.data();
        const uint64_t* __restrict qb = qbits.data();
        const int64_t n = db->n_lineitem;
        int64_t i = 0;
#ifdef __AVX2__
        // 32-bit view of the orderkey bitmap for 8-wide gather.
        const uint32_t* __restrict qb32 = reinterpret_cast<const uint32_t*>(qb);
        const __m256i vfilter = _mm256_set1_epi32(date_filter);
        const __m256i vone = _mm256_set1_epi32(1);
        const __m256i vmask5 = _mm256_set1_epi32(31);
        int32_t* __restrict hp = hits.data();
        size_t hc = 0;
        for (; i + 8 <= n; i += 8) {
            __m256i sd = _mm256_loadu_si256((const __m256i*)(l_shipdate + i));
            __m256i ship = _mm256_cmpgt_epi32(sd, vfilter);
            __m256i ok = _mm256_loadu_si256((const __m256i*)(l_orderkey + i));
            __m256i widx = _mm256_srli_epi32(ok, 5);
            __m256i words = _mm256_i32gather_epi32((const int*)qb32, widx, 4);
            __m256i bitpos = _mm256_and_si256(ok, vmask5);
            __m256i bit = _mm256_and_si256(_mm256_srlv_epi32(words, bitpos), vone);
            __m256i bitm = _mm256_cmpeq_epi32(bit, vone);
            __m256i hitv = _mm256_and_si256(ship, bitm);
            unsigned m = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(hitv));
            // hits are very rare (~0.5%), so this is almost always skipped.
            while (m) {
                int lane = __builtin_ctz(m);
                m &= m - 1;
                hp[hc++] = (int32_t)(i + lane);
            }
        }
        hits.resize(hc);
#endif
        for (; i < n; i++) {
            uint32_t ok = (uint32_t)l_orderkey[i];
            unsigned pass = (unsigned)(l_shipdate[i] > date_filter);
            unsigned hit = pass & (unsigned)((qb[ok >> 6] >> (ok & 63)) & 1);
            if (hit) hits.push_back((int32_t)i);
        }
    }
    TRACE_COUNT("q3_rows_scanned", (uint64_t)db->n_lineitem);
    TRACE_COUNT("q3_join_rows_emitted", (uint64_t)hits.size());
    TRACE_COUNT("q3_agg_rows_in", (uint64_t)hits.size());

    // Aggregate the collected hits. hit indices are increasing, so lineitem
    // rows for one order are contiguous -> run-length sum, one group lookup
    // per distinct orderkey.
    {
        const int32_t* __restrict l_orderkey = db->l_orderkey.data();
        const int64_t* __restrict l_extendedprice = db->l_extendedprice.data();
        const int64_t* __restrict l_discount = db->l_discount.data();
        const int32_t* __restrict ok2idx = db->orderkey_to_idx.data();
        const Date* __restrict o_orderdate = db->o_orderdate.data();
        const int32_t* __restrict o_shippriority = db->o_shippriority.data();
        int32_t cur_ok = -1;
        int64_t cur_rev = 0;
        for (int32_t idx : hits) {
            uint32_t ok = (uint32_t)l_orderkey[idx];
            int64_t rev = l_extendedprice[idx] * (100 - l_discount[idx]);
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
        if (cur_ok >= 0) {
            int32_t o_idx = ok2idx[cur_ok];
            results.push_back({cur_ok, cur_rev,
                               o_orderdate[o_idx], o_shippriority[o_idx]});
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
