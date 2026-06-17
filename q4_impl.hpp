#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q3_impl.hpp" // format_date
#include <ostream>
#include <vector>
#include <map>
#include <unordered_set>
#include <immintrin.h>

// 256-entry left-pack permutation table for AVX2 compaction of 8x int32 lanes.
inline const int32_t* q4_leftpack_table() {
    static int32_t tbl[256][8];
    static bool init = [] {
        for (int m = 0; m < 256; m++) {
            int pos = 0;
            for (int l = 0; l < 8; l++)
                if (m & (1 << l)) tbl[m][pos++] = l;
            for (; pos < 8; pos++) tbl[m][pos] = 0;
        }
        return true;
    }();
    (void)init;
    return &tbl[0][0];
}

inline void __attribute__((target("avx2"))) run_q4_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q4_total");
    TRACE_DECL_COUNTER(orders_scanned);
    TRACE_DECL_COUNTER(orders_emitted);
    TRACE_DECL_COUNTER(li_probed);
    // o_orderdate >= '1993-09-01' AND o_orderdate < '1993-12-01'
    const Date date_lo = date_to_epoch(1993, 9, 1);
    const Date date_hi = date_to_epoch(1993, 12, 1);

    // o_orderpriority is one of 5 values "1-URGENT".."5-LOW"; bucket by first
    // digit (1..9 -> idx 0..8) and remember the full string for output.
    int64_t cnt[10] = {0};
    const std::string* repr[10] = {nullptr};

    if (db->lineitem_sorted_by_orderkey) {
        const Date* __restrict odate = db->o_orderdate.data();
        const int32_t* __restrict okey = db->o_orderkey.data();
        const Database::LineitemRange* __restrict lrng = db->orderkey_lineitem_range.data();
        const Date* __restrict lc = db->l_commitdate.data();
        const Date* __restrict lr = db->l_receiptdate.data();

        // Phase 1: scan the date column, collecting the (sparse, ~3.8%) order
        // rows that pass the date filter. AVX2 range-check via the
        // (unsigned)(d - lo) < (hi - lo) trick + left-pack compaction.
        std::vector<int32_t> pass(db->n_orders + 8);
        int32_t* __restrict pbuf = pass.data();
        size_t npass = 0;
        {
            PROFILE_SCOPE("q4_orders_date_filter");
            const int32_t* tbl = q4_leftpack_table();
            const uint32_t range = (uint32_t)(date_hi - date_lo);
            const __m256i vlo = _mm256_set1_epi32(date_lo);
            const __m256i vsign = _mm256_set1_epi32((int)0x80000000);
            const __m256i vrange_x = _mm256_set1_epi32((int)(range ^ 0x80000000u));
            const __m256i viota = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
            int32_t i = 0;
            int32_t n8 = db->n_orders & ~7;
            for (; i < n8; i += 8) {
                __m256i d = _mm256_loadu_si256((const __m256i*)(odate + i));
                __m256i sub = _mm256_sub_epi32(d, vlo);
                __m256i sub_x = _mm256_xor_si256(sub, vsign);
                __m256i mask = _mm256_cmpgt_epi32(vrange_x, sub_x);
                unsigned m = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(mask));
                __m256i vidx = _mm256_add_epi32(_mm256_set1_epi32(i), viota);
                __m256i perm = _mm256_loadu_si256((const __m256i*)(tbl + m * 8));
                __m256i packed = _mm256_permutevar8x32_epi32(vidx, perm);
                _mm256_storeu_si256((__m256i*)(pbuf + npass), packed);
                npass += (size_t)__builtin_popcount(m);
            }
            for (; i < db->n_orders; i++) {
                Date d = odate[i];
                pbuf[npass] = i;
                npass += (size_t)((uint32_t)(d - date_lo) < range);
            }
            TRACE_ADD(orders_scanned, db->n_orders);
        }

        const int n = (int)npass;

        // Phase 2a: gather each passing order's lineitem [start,end) range into a
        // compact array. The CSR gather is the only random access here and is
        // hidden by an independent prefetch stream (no dependent loads).
        std::vector<Database::LineitemRange> rng(n);
        {
            PROFILE_SCOPE("q4_range_gather");
            const int PA = 128;
            for (int k = 0; k < n; k++) {
                if (k + PA < n) __builtin_prefetch(&lrng[okey[pbuf[k + PA]]], 0, 1);
                rng[k] = lrng[okey[pbuf[k]]];
            }
        }

        // Phase 2b: probe lineitem dates. Prefetch addresses come straight from
        // the compact range array (no dependent load), maximizing MLP.
        {
            PROFILE_SCOPE("q4_orders_scan_agg");
            const int PB = 128;
            for (int k = 0; k < n; k++) {
                if (k + PB < n) {
                    int32_t s2 = rng[k + PB].start;
                    __builtin_prefetch(&lc[s2], 0, 1);
                    __builtin_prefetch(&lr[s2], 0, 1);
                }
                Database::LineitemRange r = rng[k];
                bool late = false;
                for (int32_t j = r.start; j < r.end; j++) {
                    TRACE_INC(li_probed);
                    if (lc[j] < lr[j]) { late = true; break; }
                }
                if (late) {
                    TRACE_INC(orders_emitted);
                    const std::string& p = db->o_orderpriority[pbuf[k]];
                    int b = (int)((unsigned char)p[0] - '1');
                    cnt[b]++;
                    if (!repr[b]) repr[b] = &p;
                }
            }
        }
    } else {
        // Fallback: build late-order bitmap from full lineitem scan, then aggregate.
        std::vector<bool> order_has_late(db->max_orderkey + 1, false);
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_probed);
            if (db->l_commitdate[i] < db->l_receiptdate[i]) {
                int32_t ok = db->l_orderkey[i];
                if (ok <= db->max_orderkey) order_has_late[ok] = true;
            }
        }
        for (int32_t i = 0; i < db->n_orders; i++) {
            TRACE_INC(orders_scanned);
            if (db->o_orderdate[i] >= date_lo && db->o_orderdate[i] < date_hi) {
                if (order_has_late[db->o_orderkey[i]]) {
                    TRACE_INC(orders_emitted);
                    const std::string& p = db->o_orderpriority[i];
                    int b = (int)((unsigned char)p[0] - '1');
                    cnt[b]++;
                    if (!repr[b]) repr[b] = &p;
                }
            }
        }
    }

    TRACE_COUNT("q4_probe_rows_in", li_probed);
    TRACE_COUNT("q4_rows_scanned", orders_scanned);
    TRACE_COUNT("q4_rows_emitted", orders_emitted);

    PROFILE_SCOPE("q4_output");
    write_csv_header(out, {"o_orderpriority","order_count"});
    for (int b = 0; b < 10; b++) {
        if (repr[b]) write_csv_row(out, {*repr[b], std::to_string(cnt[b])});
    }
}
