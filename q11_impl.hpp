#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>

inline void run_q11_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q11_total");
    TRACE_DECL_COUNTER(ps_scanned);
    TRACE_DECL_COUNTER(ps_emitted);
    // n_name = 'RUSSIA' → nationkey
    int32_t russia_nk = db->nation_name_to_key["RUSSIA"];

    // Suppliers in RUSSIA
    std::unordered_set<int32_t> russia_suppkeys; // 1-based suppkeys
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (db->s_nationkey[i] == russia_nk) {
            russia_suppkeys.insert(i + 1);
        }
    }

    // Scan partsupp, filter by RUSSIA suppliers
    // Group by ps_partkey → sum(ps_supplycost * ps_availqty)
    // ps_supplycost is scale 2 (cents), ps_availqty is integer
    // product = supplycost * availqty → scale 2
    std::unordered_map<int32_t, __int128> partkey_value;
    __int128 total_value = 0;

    {
        PROFILE_SCOPE("q11_partsupp_scan_agg");
        for (int32_t i = 0; i < db->n_partsupp; i++) {
            TRACE_INC(ps_scanned);
            if (russia_suppkeys.count(db->ps_suppkey[i])) {
                TRACE_INC(ps_emitted);
                __int128 val = (__int128)db->ps_supplycost[i] * db->ps_availqty[i];
                partkey_value[db->ps_partkey[i]] += val;
                total_value += val;
            }
        }
    }
    TRACE_COUNT("q11_rows_scanned", ps_scanned);
    TRACE_COUNT("q11_rows_emitted", ps_emitted);
    TRACE_COUNT("q11_agg_rows_in", ps_emitted);
    TRACE_COUNT("q11_groups_created", (uint64_t)partkey_value.size());

    // HAVING threshold: total_value * 0.0001
    // To avoid floating point: threshold = total_value / 10000
    // But we need > not >=, so: value > total_value * 0.0001
    double threshold = (double)total_value * 0.0001;

    // Collect results that pass HAVING
    struct ResultRow {
        int32_t ps_partkey;
        long long value;
    };
    std::vector<ResultRow> results;
    for (auto& [pk, val] : partkey_value) {
        if ((double)val > threshold) {
            results.push_back({pk, static_cast<long long>(val)});
        }
    }
    TRACE_COUNT("q11_agg_rows_emitted", (uint64_t)results.size());

    // Order by value desc
    TRACE_COUNT("q11_sort_rows_in", (uint64_t)results.size());
    {
        PROFILE_SCOPE("q11_sort");
        std::sort(results.begin(), results.end(), [](const ResultRow& a, const ResultRow& b) {
            return a.value > b.value;
        });
    }
    TRACE_COUNT("q11_sort_rows_out", (uint64_t)results.size());

    PROFILE_SCOPE("q11_output");
    write_csv_header(out, {"ps_partkey","value"});
    for (auto& r : results) {
        write_csv_row(out, {
            std::to_string(r.ps_partkey),
            fmt_money(r.value, 2)
        });
    }
    TRACE_COUNT("q11_query_output_rows", (uint64_t)results.size());
}
