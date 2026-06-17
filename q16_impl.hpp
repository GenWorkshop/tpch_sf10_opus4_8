#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <unordered_set>

// q16: Parts/supplier relationship
// partsupp join part WHERE p_brand != 'Brand#31' AND p_type NOT LIKE 'SMALL POLISHED%'
// AND p_size IN (22,18,10,14,50,7,6,25)
// AND ps_suppkey NOT IN (complaint suppliers)
// Group by (p_brand, p_type, p_size), count DISTINCT ps_suppkey
// Order by supplier_cnt DESC, p_brand, p_type, p_size

static void run_q16(Database* db, const std::string& run_nr) {
    // Valid sizes
    std::unordered_set<int32_t> valid_sizes = {22, 18, 10, 14, 50, 7, 6, 25};

    // Group key -> set of distinct suppkeys
    struct Key {
        std::string brand;
        std::string type;
        int32_t size;
        bool operator<(const Key& o) const {
            if (brand != o.brand) return brand < o.brand;
            if (type != o.type) return type < o.type;
            return size < o.size;
        }
    };
    std::map<Key, std::set<int32_t>> groups;

    // Scan partsupp
    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q16_partsupp_scan_join_agg");
        for (int64_t i = 0; i < db->partsupp_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t sk = db->ps_suppkey[i];
            // Exclude complaint suppliers
            if (db->complaint_suppliers.count(sk)) continue;

            int32_t pk = db->ps_partkey[i];
            // Part filters
            if (db->p_brand[pk] == "Brand#31") continue;
            const std::string& ptype = db->p_type[pk];
            if (ptype.size() >= 14 && ptype.substr(0, 14) == "SMALL POLISHED") continue;
            if (!valid_sizes.count(db->p_size[pk])) continue;
            TRACE_INC(rows_emitted);

            Key key{db->p_brand[pk], ptype, db->p_size[pk]};
            groups[key].insert(sk);
        }
    }
    TRACE_COUNT("q16_rows_scanned", rows_scanned);
    TRACE_COUNT("q16_rows_emitted", rows_emitted);
    TRACE_COUNT("q16_groups_created", groups.size());

    // Collect results
    struct Result {
        std::string brand;
        std::string type;
        int32_t size;
        int64_t supplier_cnt;
    };
    std::vector<Result> results;
    results.reserve(groups.size());
    for (auto& [key, supps] : groups) {
        results.push_back({key.brand, key.type, key.size, (int64_t)supps.size()});
    }

    {
        PROFILE_SCOPE("q16_sort");
        // Sort by supplier_cnt DESC, brand, type, size
        std::sort(results.begin(), results.end(), [](const Result& a, const Result& b) {
            if (a.supplier_cnt != b.supplier_cnt) return a.supplier_cnt > b.supplier_cnt;
            if (a.brand != b.brand) return a.brand < b.brand;
            if (a.type != b.type) return a.type < b.type;
            return a.size < b.size;
        });
    }
    TRACE_COUNT("q16_sort_rows_in", results.size());
    TRACE_COUNT("q16_sort_rows_out", results.size());

    {
    PROFILE_SCOPE("q16_output");
    std::ostringstream oss;
    write_csv_header(oss, {"p_brand","p_type","p_size","supplier_cnt"});
    for (auto& r : results) {
        write_csv_row(oss, {
            csv_quote(r.brand),
            csv_quote(r.type),
            std::to_string(r.size),
            std::to_string(r.supplier_cnt)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q16_query_output_rows", results.size());
    }
    TRACE_FLUSH();
}
