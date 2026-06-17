#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <unordered_set>
#include <string>
#include <vector>
#include <memory>

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

    bool air_code[256] = {false};
    for (size_t d = 0; d < db->l_shipmode_dict.size(); d++)
        if (db->l_shipmode_dict[d] == "AIR" || db->l_shipmode_dict[d] == "AIR REG")
            air_code[d] = true;

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
        for (int32_t p = 0; p < n_part; p++) {
            const std::string& brand = pb[p];
            const std::string& container = pc[p];
            const int32_t sz = ps[p];
            uint8_t g = 0;
            if (brand == "Brand#14" && sz <= 5 &&
                (container == "SM CASE" || container == "SM BOX" || container == "SM PACK" || container == "SM PKG")) {
                g = 1;
            } else if (brand == "Brand#15" && sz <= 10 &&
                (container == "MED BAG" || container == "MED BOX" || container == "MED PKG" || container == "MED PACK")) {
                g = 2;
            } else if (brand == "Brand#35" && sz <= 15 &&
                (container == "LG CASE" || container == "LG BOX" || container == "LG PACK" || container == "LG PKG")) {
                g = 3;
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

        // Pass 1: branchless collection of rows passing the two single-byte
        // dictionary filters.  Writing the index unconditionally and advancing
        // the cursor by the (0/1) predicate removes the hard-to-predict branch
        // that otherwise dominates the 60M-row scan.
        std::unique_ptr<int32_t[]> cand(new int32_t[n]);
        int64_t cnt = 0;
        for (int64_t i = 0; i < n; i++) {
            const int keep = (si[i] == deliver_code) & (int)air_code[sm[i]];
            cand[cnt] = (int32_t)i;
            cnt += keep;
        }
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
