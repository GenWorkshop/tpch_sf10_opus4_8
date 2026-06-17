#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include "q7_impl.hpp" // epoch_to_year
#include <ostream>
#include <vector>
#include <cstdint>

inline void run_q8_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q8_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);

    // ---- Build side filters (small, following DuckDB's build/probe choices) ----

    // part filter: p_type = 'ECONOMY BRUSHED TIN'  → bit-packed bitmap by partkey.
    // Bit-packed so the random probe (driven by the 60M lineitem scan) stays in L2.
    const int32_t np = db->n_part;
    std::vector<uint8_t> part_match((np >> 3) + 1, 0);
    for (int32_t i = 0; i < np; i++) {
        if (db->p_type[i] == "ECONOMY BRUSHED TIN") {
            int32_t pk = i + 1; // 1-based
            part_match[pk >> 3] |= (uint8_t)(1u << (pk & 7));
        }
    }

    // region/nation filter: r_name = 'AMERICA' → set of customer nationkeys.
    int32_t america_regionkey = -1;
    for (int i = 0; i < db->n_regions; i++) {
        if (db->r_name[i] == "AMERICA") { america_regionkey = i; break; }
    }
    uint8_t nation_in_america[256] = {0};
    for (int i = 0; i < db->n_nations; i++) {
        if (db->n_regionkey[i] == america_regionkey) nation_in_america[i & 0xff] = 1;
    }

    // supplier nation = FRANCE → bit-packed bitmap by suppkey.
    int32_t france_nk = db->nation_name_to_key["FRANCE"];
    const int32_t ns = db->n_supplier;
    std::vector<uint8_t> supp_france((ns >> 3) + 1, 0);
    for (int32_t i = 0; i < ns; i++) {
        if (db->s_nationkey[i] == france_nk) {
            int32_t sk = i + 1;
            supp_france[sk >> 3] |= (uint8_t)(1u << (sk & 7));
        }
    }

    // ---- orders ⋈ customer (date range + AMERICA region) ----
    // Materialise year directly indexed by orderkey so the lineitem probe needs
    // only a single random lookup instead of orderkey→idx→year.
    const Date date_lo = date_to_epoch(1995, 1, 1);
    const Date date_hi = date_to_epoch(1996, 12, 31);
    std::vector<int8_t> oyear_by_key(db->max_orderkey + 1, -1);
    for (int32_t i = 0; i < db->n_orders; i++) {
        Date od = db->o_orderdate[i];
        if (od >= date_lo && od <= date_hi) {
            int32_t c_idx = db->o_custkey[i] - 1;
            if ((uint32_t)c_idx < (uint32_t)db->n_customer &&
                nation_in_america[db->c_nationkey[c_idx] & 0xff]) {
                // only years 1995 / 1996 fall in range → encode as 0 / 1
                oyear_by_key[db->o_orderkey[i]] =
                    (int8_t)(epoch_to_year(od) - 1995);
            }
        }
    }

    // ---- lineitem scan: part filter → order join → supplier(FRANCE) → group by year ----
    double total_volume[2] = {0.0, 0.0};
    double france_volume[2] = {0.0, 0.0};
    const int64_t n = db->n_lineitem;
    const int32_t* __restrict lp = db->l_partkey.data();
    const int32_t* __restrict lo = db->l_orderkey.data();
    const int32_t* __restrict lsk = db->l_suppkey.data();
    const int64_t* __restrict lep = db->l_extendedprice.data();
    const int64_t* __restrict ld = db->l_discount.data();
    const uint8_t* __restrict pm = part_match.data();
    const int8_t* __restrict oy = oyear_by_key.data();
    const uint8_t* __restrict sf = supp_france.data();
    const int32_t max_ok = db->max_orderkey;

    {
        PROFILE_SCOPE("q8_lineitem_scan_join_agg");
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            int32_t pk = lp[i];
            if (!((pm[pk >> 3] >> (pk & 7)) & 1)) continue;

            int32_t ok = lo[i];
            if ((uint32_t)ok > (uint32_t)max_ok) continue;
            int8_t yr = oy[ok];
            if (yr < 0) continue;

            TRACE_INC(li_emitted);
            double volume = (double)lep[i] * (double)(100 - ld[i]);
            total_volume[yr] += volume;

            int32_t sk = lsk[i];
            uint32_t isfr = (sf[sk >> 3] >> (sk & 7)) & 1;
            france_volume[yr] += isfr ? volume : 0.0;
        }
    }

    TRACE_COUNT("q8_rows_scanned", li_scanned);
    TRACE_COUNT("q8_join_rows_emitted", li_emitted);
    TRACE_COUNT("q8_agg_rows_in", li_emitted);
    TRACE_COUNT("q8_groups_created", 2);
    TRACE_COUNT("q8_agg_rows_emitted", 2);

    PROFILE_SCOPE("q8_output");
    write_csv_header(out, {"o_year","mkt_share"});
    int rows_out = 0;
    for (int y = 0; y < 2; y++) {
        if (total_volume[y] == 0.0 && france_volume[y] == 0.0) continue;
        double mkt_share = (total_volume[y] == 0.0) ? 0.0
                            : france_volume[y] / total_volume[y];
        write_csv_row(out, {
            std::to_string(1995 + y),
            fmt_decimal(mkt_share, 15)
        });
        rows_out++;
    }
    TRACE_COUNT("q8_query_output_rows", (uint64_t)rows_out);
}
