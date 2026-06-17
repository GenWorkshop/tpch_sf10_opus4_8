#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "q1_impl.hpp" // date_to_epoch
#include <ostream>
#include <vector>
#include <map>

// Extract year from epoch days
inline int32_t epoch_to_year(int32_t days) {
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
    int y = static_cast<int>(yoe) + era * 400;
    unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
    unsigned mp = (5*doy + 2) / 153;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    return y;
}

inline void run_q7_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q7_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
    // Find nationkeys for ALGERIA and BRAZIL
    int32_t nk_algeria = db->nation_name_to_key["ALGERIA"];
    int32_t nk_brazil = db->nation_name_to_key["BRAZIL"];

    // l_shipdate between '1995-01-01' and '1996-12-31'
    const Date date_lo = date_to_epoch(1995, 1, 1);
    const Date date_hi = date_to_epoch(1996, 12, 31);

    // For each order, get customer nationkey
    // order_idx → c_nationkey (precompute for qualifying customers)
    // We only care about orders where customer is ALGERIA or BRAZIL
    std::vector<int8_t> order_cust_nation(db->n_orders, -1); // -1=not relevant, 0=ALGERIA, 1=BRAZIL
    for (int32_t i = 0; i < db->n_orders; i++) {
        int32_t custkey = db->o_custkey[i];
        int32_t c_idx = custkey - 1;
        if (c_idx >= 0 && c_idx < db->n_customer) {
            int32_t cn = db->c_nationkey[c_idx];
            if (cn == nk_algeria) order_cust_nation[i] = 0;
            else if (cn == nk_brazil) order_cust_nation[i] = 1;
        }
    }

    // Group key: (supp_nation_name, cust_nation_name, year) → revenue
    // Use tuple (supp_idx, cust_idx, year) where idx: 0=ALGERIA, 1=BRAZIL
    struct Key {
        int8_t supp_nation; // 0=ALGERIA, 1=BRAZIL
        int8_t cust_nation;
        int32_t year;
        bool operator<(const Key& o) const {
            if (supp_nation != o.supp_nation) return supp_nation < o.supp_nation;
            if (cust_nation != o.cust_nation) return cust_nation < o.cust_nation;
            return year < o.year;
        }
    };
    std::map<Key, int64_t> groups;

    {
        PROFILE_SCOPE("q7_lineitem_scan_join_agg");
        for (int64_t i = 0; i < db->n_lineitem; i++) {
            TRACE_INC(li_scanned);
            if (db->l_shipdate[i] < date_lo || db->l_shipdate[i] > date_hi) continue;

            // Get supplier nation
            int32_t suppkey = db->l_suppkey[i];
            int32_t s_idx = suppkey - 1;
            if (s_idx < 0 || s_idx >= db->n_supplier) continue;
            int32_t s_nation = db->s_nationkey[s_idx];
            int8_t sn = -1;
            if (s_nation == nk_algeria) sn = 0;
            else if (s_nation == nk_brazil) sn = 1;
            else continue;

            // Get order → customer nation
            int32_t orderkey = db->l_orderkey[i];
            if (orderkey > db->max_orderkey) continue;
            int32_t o_idx = db->orderkey_to_idx[orderkey];
            if (o_idx < 0) continue;
            int8_t cn = order_cust_nation[o_idx];
            if (cn < 0) continue;

            // Filter: (ALGERIA,BRAZIL) or (BRAZIL,ALGERIA)
            if (!((sn == 0 && cn == 1) || (sn == 1 && cn == 0))) continue;

            TRACE_INC(li_emitted);
            int32_t year = epoch_to_year(db->l_shipdate[i]);
            int64_t rev = db->l_extendedprice[i] * (100 - db->l_discount[i]); // scale 4

            groups[{sn, cn, year}] += rev;
        }
    }
    TRACE_COUNT("q7_rows_scanned", li_scanned);
    TRACE_COUNT("q7_join_rows_emitted", li_emitted);
    TRACE_COUNT("q7_agg_rows_in", li_emitted);
    TRACE_COUNT("q7_groups_created", (uint64_t)groups.size());
    TRACE_COUNT("q7_agg_rows_emitted", (uint64_t)groups.size());

    PROFILE_SCOPE("q7_output");
    write_csv_header(out, {"supp_nation","cust_nation","l_year","revenue"});

    const char* names[] = {"ALGERIA", "BRAZIL"};
    for (auto& [k, rev] : groups) {
        write_csv_row(out, {
            std::string(names[k.supp_nation]),
            std::string(names[k.cust_nation]),
            std::to_string(k.year),
            fmt_money(rev, 4)
        });
    }
    TRACE_COUNT("q7_query_output_rows", (uint64_t)groups.size());
}
