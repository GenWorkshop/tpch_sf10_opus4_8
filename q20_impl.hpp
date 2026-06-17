#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <immintrin.h>

// Flat open-addressing hash table for (partkey,suppkey) -> sum(l_quantity).
// Empty slots are marked by key == 0 (real keys are pk*1e6+sk >= 1e6+1).
struct Q20Agg {
    std::vector<int64_t> key;
    std::vector<int64_t> val;
    uint64_t mask;
    Q20Agg(size_t cap_pow2) : key(cap_pow2, 0), val(cap_pow2, 0), mask(cap_pow2 - 1) {}
    static inline uint64_t hash(int64_t k) {
        uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ULL;
        return x >> 32;
    }
    inline void add(int64_t k, int64_t q) {
        uint64_t h = hash(k) & mask;
        while (true) {
            int64_t cur = key[h];
            if (cur == 0) { key[h] = k; val[h] = q; return; }
            if (cur == k) { val[h] += q; return; }
            h = (h + 1) & mask;
        }
    }
    // returns pointer to value or nullptr
    inline const int64_t* find(int64_t k) const {
        uint64_t h = hash(k) & mask;
        while (true) {
            int64_t cur = key[h];
            if (cur == 0) return nullptr;
            if (cur == k) return &val[h];
            h = (h + 1) & mask;
        }
    }
};

__attribute__((target("avx2")))
static int64_t q20_lineitem_avx2(const Date* __restrict shipdate,
                                 const int32_t* __restrict lpartkey,
                                 const int32_t* __restrict lsuppkey,
                                 const int64_t* __restrict lqty,
                                 int64_t n,
                                 Date date_lo, Date date_hi,
                                 const uint64_t* __restrict linen_bits,
                                 Q20Agg& agg) {
    int64_t emitted = 0;
    const __m256i vlo = _mm256_set1_epi32(date_lo - 1); // d > lo-1  <=> d >= lo
    const __m256i vhi = _mm256_set1_epi32(date_hi);     // hi > d    <=> d < hi
    int64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i d  = _mm256_loadu_si256((const __m256i*)(shipdate + i));
        __m256i ge = _mm256_cmpgt_epi32(d, vlo);
        __m256i lt = _mm256_cmpgt_epi32(vhi, d);
        __m256i m  = _mm256_and_si256(ge, lt);
        unsigned mask = (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(m));
        while (mask) {
            int lane = __builtin_ctz(mask);
            mask &= mask - 1;
            int64_t idx = i + lane;
            int32_t pk = lpartkey[idx];
            if ((linen_bits[(uint32_t)pk >> 6] >> ((uint32_t)pk & 63)) & 1ULL) {
                int64_t k = (int64_t)pk * 1000000LL + lsuppkey[idx];
                agg.add(k, lqty[idx]);
                emitted++;
            }
        }
    }
    for (; i < n; i++) {
        Date dd = shipdate[i];
        if (dd >= date_lo && dd < date_hi) {
            int32_t pk = lpartkey[i];
            if ((linen_bits[(uint32_t)pk >> 6] >> ((uint32_t)pk & 63)) & 1ULL) {
                int64_t k = (int64_t)pk * 1000000LL + lsuppkey[i];
                agg.add(k, lqty[i]);
                emitted++;
            }
        }
    }
    return emitted;
}

inline void run_q20_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q20_total");
    TRACE_DECL_COUNTER(ps_scanned);
    // n_name = 'FRANCE' → nationkey
    int32_t france_nk = db->nation_name_to_key["FRANCE"];

    // Parts where p_name like 'linen%' → cache-resident bitmap indexed by partkey (1-based)
    const int32_t n_part = db->n_part;
    std::vector<uint64_t> linen_bits((size_t)(n_part + 1) / 64 + 1, 0);
    for (int32_t i = 0; i < n_part; i++) {
        const std::string& nm = db->p_name[i];
        if (nm.size() >= 5 && std::memcmp(nm.data(), "linen", 5) == 0) {
            int32_t pk = i + 1;
            linen_bits[(uint32_t)pk >> 6] |= (1ULL << ((uint32_t)pk & 63));
        }
    }
    auto is_linen = [&](int32_t pk) -> bool {
        return (linen_bits[(uint32_t)pk >> 6] >> ((uint32_t)pk & 63)) & 1ULL;
    };

    // Compute sum(l_quantity) per (partkey, suppkey) for shipdate in 1997
    const Date date_lo = date_to_epoch(1997, 1, 1);
    const Date date_hi = date_to_epoch(1998, 1, 1);

    Q20Agg agg(1 << 18); // ~59k distinct groups → load factor ~0.22
    {
        PROFILE_SCOPE("q20_lineitem_scan_agg");
        int64_t emitted = q20_lineitem_avx2(
            db->l_shipdate.data(), db->l_partkey.data(), db->l_suppkey.data(),
            db->l_quantity.data(), db->n_lineitem, date_lo, date_hi,
            linen_bits.data(), agg);
        TRACE_COUNT("q20_rows_scanned", (uint64_t)db->n_lineitem);
        TRACE_COUNT("q20_rows_emitted", (uint64_t)emitted);
    }

    // Find qualifying suppkeys: partsupp where ps_partkey in linen_parts
    // and ps_availqty > 0.5 * sum(l_quantity)
    std::unordered_set<int32_t> qualifying_suppkeys;
    {
        PROFILE_SCOPE("q20_partsupp_scan_join");
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            TRACE_INC(ps_scanned);
            int32_t pk = db->ps_partkey[i];
            if (!is_linen(pk)) continue;

            int64_t key = (int64_t)pk * 1000000LL + db->ps_suppkey[i];
            const int64_t* it = agg.find(key);
            if (!it) continue; // no lineitem → NULL → skip

            int64_t sum_qty = *it;

            // ps_availqty * 200 > sum_qty
            if ((int64_t)db->ps_availqty[i] * 200 > sum_qty) {
                qualifying_suppkeys.insert(db->ps_suppkey[i]);
            }
        }
    }
    TRACE_COUNT("q20_partsupp_rows_scanned", ps_scanned);
    TRACE_COUNT("q20_qualifying_suppkeys", (uint64_t)qualifying_suppkeys.size());

    // Find suppliers in FRANCE that are in qualifying set
    struct ResultRow {
        std::string s_name;
        std::string s_address;
    };
    std::vector<ResultRow> results;
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (db->s_nationkey[i] == france_nk && qualifying_suppkeys.count(i + 1)) {
            results.push_back({db->s_name[i], db->s_address[i]});
        }
    }

    // Order by s_name
    TRACE_COUNT("q20_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q20_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.s_name < b.s_name;
        });
    }
    TRACE_COUNT("q20_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q20_output");
    write_csv_header(out, {"s_name","s_address"});
    for (auto& r : results) {
        write_csv_row(out, {csv_quote(r.s_name), csv_quote(r.s_address)});
    }
    TRACE_COUNT("q20_query_output_rows", (uint64_t)results.size());
}
