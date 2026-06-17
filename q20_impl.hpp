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

inline void run_q20_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q20_total");
    TRACE_DECL_COUNTER(li_scanned);
    TRACE_DECL_COUNTER(li_emitted);
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

    std::unordered_map<int64_t, int64_t> ps_qty_sum; // key = partkey*1000000 + suppkey → sum qty (scale 2)
    ps_qty_sum.reserve(1 << 17);
    {
        PROFILE_SCOPE("q20_lineitem_scan_agg");
        const Date* __restrict shipdate = db->l_shipdate.data();
        const int32_t* __restrict lpartkey = db->l_partkey.data();
        const int32_t* __restrict lsuppkey = db->l_suppkey.data();
        const int64_t* __restrict lqty = db->l_quantity.data();
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(li_scanned);
            Date d = shipdate[i];
            if (d >= date_lo && d < date_hi) {
                int32_t pk = lpartkey[i];
                if (is_linen(pk)) {
                    TRACE_INC(li_emitted);
                    int64_t key = (int64_t)pk * 1000000LL + lsuppkey[i];
                    ps_qty_sum[key] += lqty[i];
                }
            }
        }
    }
    TRACE_COUNT("q20_rows_scanned", li_scanned);
    TRACE_COUNT("q20_rows_emitted", li_emitted);
    TRACE_COUNT("q20_groups_created", (uint64_t)ps_qty_sum.size());

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
            auto it = ps_qty_sum.find(key);
            if (it == ps_qty_sum.end()) continue; // no lineitem → NULL → skip

            int64_t sum_qty = it->second;

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
