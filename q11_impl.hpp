#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <vector>
#include <algorithm>

inline void run_q11_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q11_total");
    TRACE_DECL_COUNTER(ps_scanned);
    TRACE_DECL_COUNTER(ps_emitted);
    // n_name = 'RUSSIA' -> nationkey
    int32_t russia_nk = db->nation_name_to_key["RUSSIA"];

    // Dense membership bitmap: suppkey (1-based) -> in RUSSIA?
    std::vector<uint8_t> supp_in_russia(db->n_supplier + 1, 0);
    for (int32_t i = 0; i < db->n_supplier; i++) {
        if (db->s_nationkey[i] == russia_nk) {
            supp_in_russia[i + 1] = 1;
        }
    }

    // Dense per-partkey accumulator (partkey is 1-based dense).
    // product = ps_supplycost(scale 2) * ps_availqty(int) -> scale 2.
    std::vector<__int128> partkey_value(db->n_part + 1, 0);
    std::vector<int32_t> touched;
    touched.reserve(db->n_partsupp / 20 + 16);
    __int128 total_value = 0;

    {
        PROFILE_SCOPE("q11_partsupp_scan_agg");
        const int32_t* __restrict ps_suppkey = db->ps_suppkey.data();
        const int32_t* __restrict ps_partkey = db->ps_partkey.data();
        const int32_t* __restrict ps_availqty = db->ps_availqty.data();
        const int64_t* __restrict ps_supplycost = db->ps_supplycost.data();
        const uint8_t* __restrict mem = supp_in_russia.data();
        __int128* __restrict acc = partkey_value.data();
        const int32_t n = db->n_partsupp;
        for (int32_t i = 0; i < n; i++) {
            TRACE_INC(ps_scanned);
            if (mem[ps_suppkey[i]]) {
                TRACE_INC(ps_emitted);
                __int128 val = (__int128)ps_supplycost[i] * ps_availqty[i];
                int32_t pk = ps_partkey[i];
                __int128 prev = acc[pk];
                if (prev == 0) touched.push_back(pk);
                acc[pk] = prev + val;
                total_value += val;
            }
        }
    }
    TRACE_COUNT("q11_rows_scanned", ps_scanned);
    TRACE_COUNT("q11_rows_emitted", ps_emitted);
    TRACE_COUNT("q11_agg_rows_in", ps_emitted);
    TRACE_COUNT("q11_groups_created", (uint64_t)touched.size());

    // HAVING: value > total_value * 0.0001  <=>  value * 10000 > total_value
    struct ResultRow {
        int32_t ps_partkey;
        long long value;
    };
    std::vector<ResultRow> results;
    results.reserve(touched.size());
    for (int32_t pk : touched) {
        __int128 val = partkey_value[pk];
        if (val * 10000 > total_value) {
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
