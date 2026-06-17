#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

inline void run_q17_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q17_total");
    TRACE_DECL_COUNTER(scan1);
    TRACE_DECL_COUNTER(scan1_emit);
    TRACE_DECL_COUNTER(scan2);
    TRACE_DECL_COUNTER(scan2_emit);
    // Find parts: p_brand = 'Brand#13' AND p_container = 'MED BOX'.
    // Build a dense-index map over partkeys so all per-part state lives in
    // compact arrays addressed directly by partkey (no hashing on the hot path).
    const int32_t np = db->n_part;
    std::vector<int32_t> part_dense(np + 1, -1); // partkey(1-based) -> dense slot
    // Compact 1-bit-per-part presence filter (~np/8 bytes) so the 60M-row hot
    // scan probes a small L2-resident bitmap instead of the 8MB dense array;
    // the wide array is only touched on the rare (~0.1%) match.
    std::vector<uint64_t> match_bits((np >> 6) + 2, 0);
    int32_t n_match = 0;
    for (int32_t i = 0; i < np; i++) {
        if (db->p_brand[i] == "Brand#13" && db->p_container[i] == "MED BOX") {
            int32_t pk = i + 1;
            part_dense[pk] = n_match++;
            match_bits[pk >> 6] |= (uint64_t)1 << (pk & 63);
        }
    }

    // Per matching-part accumulators (avg(l_quantity) = sum_qty / count).
    std::vector<int64_t> sum_qty(n_match, 0);
    std::vector<int32_t> cnt(n_match, 0);

    // Pass 1: single scan over lineitem. For matching parts, accumulate the
    // running sum/count AND materialize the surviving (dense, qty, price)
    // tuples so pass 2 only revisits the ~join-cardinality rows, not all 60M.
    std::vector<int32_t> sel_dense;
    std::vector<int32_t> sel_qty;
    std::vector<int64_t> sel_price;
    {
        PROFILE_SCOPE("q17_lineitem_scan_agg");
        const int32_t* __restrict lp = db->l_partkey.data();
        const int64_t* __restrict lq = db->l_quantity.data();
        const int64_t* __restrict le = db->l_extendedprice.data();
        const uint64_t* __restrict mb = match_bits.data();
        const int64_t n = db->n_lineitem;
        sel_dense.reserve(1u << 21);
        sel_qty.reserve(1u << 21);
        sel_price.reserve(1u << 21);
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(scan1);
            uint32_t pk = (uint32_t)lp[i];
            if ((mb[pk >> 6] >> (pk & 63)) & 1) {
                TRACE_INC(scan1_emit);
                int32_t d = part_dense[pk];
                int32_t q = (int32_t)lq[i];
                sum_qty[d] += q;
                cnt[d]++;
                sel_dense.push_back(d);
                sel_qty.push_back(q);
                sel_price.push_back(le[i]);
            }
        }
    }
    TRACE_COUNT("q17_rows_scanned", scan1);
    TRACE_COUNT("q17_rows_emitted", scan1_emit);
    TRACE_COUNT("q17_groups_created", (uint64_t)n_match);

    // Pass 2: sum extendedprice where l_quantity < 0.2 * avg(l_quantity).
    // Equivalent integer test: l_quantity * count * 5 < sum_qty.
    int64_t total_price = 0;
    {
        PROFILE_SCOPE("q17_lineitem_scan_filter");
        const size_t m = sel_dense.size();
        const int32_t* __restrict sd = sel_dense.data();
        const int32_t* __restrict sq = sel_qty.data();
        const int64_t* __restrict sp = sel_price.data();
        for (size_t i = 0; i < m; i++) {
            TRACE_INC(scan2);
            int32_t d = sd[i];
            if ((int64_t)sq[i] * cnt[d] * 5 < sum_qty[d]) {
                TRACE_INC(scan2_emit);
                total_price += sp[i];
            }
        }
    }
    TRACE_COUNT("q17_rows_scanned_pass2", scan2);
    TRACE_COUNT("q17_rows_emitted_pass2", scan2_emit);
    TRACE_COUNT("q17_agg_rows_in", scan2_emit);

    // Result: sum(l_extendedprice) / 7.0
    // total_price is in scale 2 (cents), divide by 100 to get dollars, then /7
    double avg_yearly = ((double)total_price / 100.0) / 7.0;

    PROFILE_SCOPE("q17_output");
    write_csv_header(out, {"avg_yearly"});
    write_csv_row(out, {fmt_decimal(avg_yearly, 15)});
    TRACE_COUNT("q17_query_output_rows", 1);
}
