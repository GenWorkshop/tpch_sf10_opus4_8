#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <string>
#include <vector>
#include <cstring>

inline void run_q14_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q14_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // l_shipdate >= '1995-05-01' AND l_shipdate < '1995-06-01'
    const Date date_lo = date_to_epoch(1995, 5, 1);
    const Date date_hi = date_to_epoch(1995, 6, 1);

    // Build side of the hash join (part): collapse the p_type "PROMO" prefix
    // test into a dense byte array so the hot probe loop does a single cheap-
    // free lookup instead of a per-survivor std::string substring compare.
    const int32_t n_part = db->n_part;
    std::vector<uint8_t> is_promo(n_part);
    {
        PROFILE_SCOPE("q14_part_build");
        for (int32_t p = 0; p < n_part; p++) {
            const std::string& ptype = db->p_type[p];
            is_promo[p] = (ptype.size() >= 5 && std::memcmp(ptype.data(), "PROMO", 5) == 0) ? 1 : 0;
        }
    }

    // Exact integer accumulation: revenue = extendedprice(cents) * (100 - discount%).
    // Magnitudes (~1e9 per row * 12M rows ~ 1.2e16) stay within int64.
    int64_t promo_sum = 0;
    int64_t total_sum = 0;

    {
        PROFILE_SCOPE("q14_lineitem_scan_join_agg");
        const int64_t n = db->n_lineitem;
        const Date* __restrict shipdate = db->l_shipdate.data();
        const int32_t* __restrict partkey = db->l_partkey.data();
        const int64_t* __restrict price = db->l_extendedprice.data();
        const int64_t* __restrict disc = db->l_discount.data();
        const uint8_t* __restrict promo = is_promo.data();
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            if (shipdate[i] >= date_lo && shipdate[i] < date_hi) {
                TRACE_INC(li_emitted);
                int64_t rev = price[i] * (100 - disc[i]);
                total_sum += rev;
                int32_t p_idx = partkey[i] - 1;
                promo_sum += rev * (int64_t)promo[p_idx];
            }
        }
    }
    TRACE_COUNT("q14_rows_scanned", li_scanned);
    TRACE_COUNT("q14_rows_emitted", li_emitted);
    TRACE_COUNT("q14_agg_rows_in", li_emitted);

    double promo_revenue = (total_sum == 0) ? 0.0 : 100.0 * (double)promo_sum / (double)total_sum;

    PROFILE_SCOPE("q14_output");
    write_csv_header(out, {"promo_revenue"});
    write_csv_row(out, {fmt_decimal(promo_revenue, 15)});
    TRACE_COUNT("q14_query_output_rows", 1);
}
