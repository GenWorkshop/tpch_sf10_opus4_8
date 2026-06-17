#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// q11: Important stock identification - partsupp join supplier join nation(RUSSIA)
// Group by ps_partkey, value = sum(ps_supplycost * ps_availqty)
// HAVING value > sum_total * 0.0001
// Order by value DESC

static void run_q11(Database* db, const std::string& run_nr) {
    // Find RUSSIA nationkey
    int32_t russia_nk = -1;
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        if (db->n_name[i] == "RUSSIA") { russia_nk = db->n_nationkey[i]; break; }
    }

    // Get suppliers in RUSSIA
    std::unordered_set<int32_t> russia_supps;
    for (int32_t sk : db->nation_to_suppliers[russia_nk]) {
        russia_supps.insert(sk);
    }

    // Scan partsupp: filter by Russia suppliers
    // Group by ps_partkey, sum(ps_supplycost * ps_availqty)
    // ps_supplycost is scale2 (cents), ps_availqty is integer
    // value = supplycost * availqty -> scale2
    std::unordered_map<int32_t, int64_t> partkey_value;
    int64_t total_value = 0;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q11_partsupp_scan_agg");
        for (int64_t i = 0; i < db->partsupp_count; i++) {
            TRACE_INC(rows_scanned);
            if (!russia_supps.count(db->ps_suppkey[i])) continue;
            TRACE_INC(rows_emitted);
            int64_t val = db->ps_supplycost[i] * (int64_t)db->ps_availqty[i];
            partkey_value[db->ps_partkey[i]] += val;
            total_value += val;
        }
    }
    TRACE_COUNT("q11_rows_scanned", rows_scanned);
    TRACE_COUNT("q11_rows_emitted", rows_emitted);
    TRACE_COUNT("q11_groups_created", partkey_value.size());

    // Threshold = total_value * 0.0001
    double threshold = total_value * 0.0001;

    // Collect results exceeding threshold
    struct Result { int32_t partkey; int64_t value; };
    std::vector<Result> results;
    for (auto& [pk, val] : partkey_value) {
        if ((double)val > threshold) {
            results.push_back({pk, val});
        }
    }

    {
        PROFILE_SCOPE("q11_sort");
        // Sort by value DESC
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            return a.value > b.value;
        });
    }
    TRACE_COUNT("q11_sort_rows_in", results.size());
    TRACE_COUNT("q11_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q11_output");
    std::ostringstream oss;
    write_csv_header(oss, {"ps_partkey","value"});
    for (auto& r : results) {
        write_csv_row(oss, {
            std::to_string(r.partkey),
            fmt_decimal(r.value / 100.0, 2)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q11_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
