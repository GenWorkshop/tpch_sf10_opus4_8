#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <unordered_set>
#include <string>
#include <vector>
#include <memory>
#include <immintrin.h>

// SIMD pass-1 for q19: scan the two single-byte dictionary columns 32 lanes at
// a time, testing (l_shipinstruct == DELIVER) AND (l_shipmode IN air codes),
// and left-pack the surviving row indices.  Survivors are sparse (~7%), so the
// bit-iteration inner loop is cheap while the 60M-row scan stays vectorized.
__attribute__((target("avx2")))
static int64_t q19_pass1_avx2(const uint8_t* __restrict si,
                              const uint8_t* __restrict sm,
                              int64_t n, int deliver_code,
                              int aira, int airb,
                              int32_t* __restrict cand) {
    const __m256i vdel  = _mm256_set1_epi8((char)deliver_code);
    const __m256i vaira = _mm256_set1_epi8((char)aira);
    const __m256i vairb = _mm256_set1_epi8((char)airb);
    int64_t cnt = 0;
    int64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i s = _mm256_loadu_si256((const __m256i*)(si + i));
        __m256i m = _mm256_loadu_si256((const __m256i*)(sm + i));
        __m256i md = _mm256_cmpeq_epi8(s, vdel);
        __m256i ma = _mm256_or_si256(_mm256_cmpeq_epi8(m, vaira),
                                     _mm256_cmpeq_epi8(m, vairb));
        uint32_t bits = (uint32_t)_mm256_movemask_epi8(_mm256_and_si256(md, ma));
        while (bits) {
            int b = __builtin_ctz(bits);
            cand[cnt++] = (int32_t)(i + b);
            bits &= bits - 1;
        }
    }
    for (; i < n; i++) {
        const int keep = (si[i] == deliver_code) &
                         ((sm[i] == aira) | (sm[i] == airb));
        cand[cnt] = (int32_t)i;
        cnt += keep;
    }
    return cnt;
}

inline void run_q19_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q19_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Three OR conditions, all require:
    //   p_partkey = l_partkey
    //   l_shipmode in ('AIR', 'AIR REG')
    //   l_shipinstruct = 'DELIVER IN PERSON'
    
    // Quantities are scale 2
    // Group 1: Brand#14, SM containers, qty 1-11, size 1-5
    // Group 2: Brand#15, MED containers, qty 17-27, size 1-10
    // Group 3: Brand#35, LG containers, qty 28-38, size 1-15

    __int128 revenue = 0;

    // Resolve dictionary codes for the constant filter values at query time.
    int deliver_code = -1;
    for (size_t d = 0; d < db->l_shipinstruct_dict.size(); d++)
        if (db->l_shipinstruct_dict[d] == "DELIVER IN PERSON") deliver_code = (int)d;

    int aira = -1, airb = -1;
    for (size_t d = 0; d < db->l_shipmode_dict.size(); d++)
        if (db->l_shipmode_dict[d] == "AIR" || db->l_shipmode_dict[d] == "AIR REG") {
            if (aira < 0) aira = (int)d; else airb = (int)d;
        }
    if (aira < 0) aira = 255;          // no AIR code present → never matches
    if (airb < 0) airb = aira;
    if (deliver_code < 0) deliver_code = 255;  // sentinel, never matches a code

    // Build side of the hash join (mirrors the plan's HASH_JOIN on `part`):
    // scan the 2M-row part table once and reduce each part to a single group
    // tag (1/2/3, or 0 = non-matching).  All the std::string brand/container
    // comparisons happen here, off the 60M-row probe path.  The resulting
    // ~2MB byte array stays L2-resident for cheap random probing.
    const int32_t n_part = db->n_part;
    std::vector<uint8_t> pgroup(n_part, 0);
    {
        PROFILE_SCOPE("q19_part_build");
        const std::string* __restrict pb = db->p_brand.data();
        const std::string* __restrict pc = db->p_container.data();
        const int32_t* __restrict ps = db->p_size.data();

        // p_brand is always exactly "Brand#NN" (8 bytes, SSO-inline) and every
        // relevant p_container is <=8 bytes, so pack them into a single uint64
        // word and compare with integer ops instead of std::string::operator==.
        auto pack = [](const char* s, size_t n) -> uint64_t {
            uint64_t w = 0; std::memcpy(&w, s, n); return w;
        };
        const uint64_t B14 = pack("Brand#14", 8);
        const uint64_t B15 = pack("Brand#15", 8);
        const uint64_t B35 = pack("Brand#35", 8);
        const uint64_t SM0 = pack("SM CASE", 7), SM1 = pack("SM BOX", 6),
                       SM2 = pack("SM PACK", 7), SM3 = pack("SM PKG", 6);
        const uint64_t MD0 = pack("MED BAG", 7), MD1 = pack("MED BOX", 7),
                       MD2 = pack("MED PKG", 7), MD3 = pack("MED PACK", 8);
        const uint64_t LG0 = pack("LG CASE", 7), LG1 = pack("LG BOX", 6),
                       LG2 = pack("LG PACK", 7), LG3 = pack("LG PKG", 6);

        for (int32_t p = 0; p < n_part; p++) {
            uint64_t bw; std::memcpy(&bw, pb[p].data(), 8);
            const int32_t sz = ps[p];
            const size_t cn = pc[p].size();
            uint8_t g = 0;
            // Containers longer than 8 bytes cannot match any target value.
            if (cn <= 8) {
                uint64_t cw = pack(pc[p].data(), cn);
                if (bw == B14) {
                    if (sz <= 5 && (cw == SM0 || cw == SM1 || cw == SM2 || cw == SM3)) g = 1;
                } else if (bw == B15) {
                    if (sz <= 10 && (cw == MD0 || cw == MD1 || cw == MD2 || cw == MD3)) g = 2;
                } else if (bw == B35) {
                    if (sz <= 15 && (cw == LG0 || cw == LG1 || cw == LG2 || cw == LG3)) g = 3;
                }
            }
            pgroup[p] = g;
        }
    }

    // Per-group l_quantity range (scale 2), indexed by group tag.
    static const int64_t qlo[4] = {0, 100, 1700, 2800};
    static const int64_t qhi[4] = {0, 1100, 2700, 3800};

    {
        PROFILE_SCOPE("q19_lineitem_scan_join_filter");
        const uint8_t* __restrict si = db->l_shipinstruct_code.data();
        const uint8_t* __restrict sm = db->l_shipmode_code.data();
        const int32_t* __restrict lpk = db->l_partkey.data();
        const int64_t* __restrict lqty = db->l_quantity.data();
        const int64_t* __restrict lprice = db->l_extendedprice.data();
        const int64_t* __restrict ldisc = db->l_discount.data();
        const uint8_t* __restrict pg = pgroup.data();
        const int64_t n = db->n_lineitem;

        // Pass 1: SIMD-vectorized branchless collection of rows passing the two
        // single-byte dictionary filters.
        std::unique_ptr<int32_t[]> cand(new int32_t[n]);
        int64_t cnt = q19_pass1_avx2(si, sm, n, deliver_code, aira, airb, cand.get());
        TRACE_ADD(li_scanned, (uint64_t)n);

        // Pass 2: probe the L2-resident part-group table for the surviving
        // ~7% of rows and apply the per-group quantity range.
        constexpr int64_t PF = 32;
        for (int64_t s = 0; s < cnt; s++) {
            if (s + PF < cnt) {
                const uint32_t pj = (uint32_t)(lpk[cand[s + PF]] - 1);
                if (pj < (uint32_t)n_part) __builtin_prefetch(&pg[pj], 0, 0);
            }
            const int32_t i = cand[s];
            const uint32_t p_idx = (uint32_t)(lpk[i] - 1);
            if (p_idx >= (uint32_t)n_part) continue;
            const uint8_t g = pg[p_idx];
            if (!g) continue;
            const int64_t qty = lqty[i];
            if (qty < qlo[g] || qty > qhi[g]) continue;
            TRACE_INC(li_emitted);
            revenue += (__int128)lprice[i] * (100 - ldisc[i]);
        }
    }
    TRACE_COUNT("q19_rows_scanned", li_scanned);
    TRACE_COUNT("q19_rows_emitted", li_emitted);
    TRACE_COUNT("q19_agg_rows_in", li_emitted);

    PROFILE_SCOPE("q19_output");
    write_csv_header(out, {"revenue"});
    write_csv_row(out, {fmt_money(static_cast<long long>(revenue), 4)});
    TRACE_COUNT("q19_query_output_rows", 1);
}
