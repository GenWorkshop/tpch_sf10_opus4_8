#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <sstream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// q17: Small-quantity-order revenue
// part(brand='Brand#13', container='MED BOX') join lineitem
// WHERE l_quantity < 0.2 * avg(l_quantity) for that part
// Result: sum(l_extendedprice) / 7.0

static void run_q17(Database* db, const std::string& run_nr) {
    // Find parts with brand='Brand#13' and container='MED BOX'
    std::unordered_set<int32_t> matching_parts;
    for (int32_t pk = 1; pk < (int32_t)db->p_brand.size(); pk++) {
        if (db->p_brand[pk] == "Brand#13" && db->p_container[pk] == "MED BOX") {
            matching_parts.insert(pk);
        }
    }

    // For each matching part, compute avg(l_quantity)
    // l_quantity is scale 2
    struct PartStats { int64_t sum_qty = 0; int64_t count = 0; };
    std::unordered_map<int32_t, PartStats> part_stats;

    for (int64_t i = 0; i < db->lineitem_count; i++) {
        int32_t pk = db->l_partkey[i];
        if (matching_parts.count(pk)) {
            part_stats[pk].sum_qty += db->l_quantity[i];
            part_stats[pk].count++;
        }
    }

    // Compute 0.2 * avg for each part
    std::unordered_map<int32_t, double> part_threshold;
    for (auto& [pk, stats] : part_stats) {
        if (stats.count > 0) {
            double avg_qty = (double)stats.sum_qty / stats.count;
            part_threshold[pk] = 0.2 * avg_qty;
        }
    }

    // Second pass: sum l_extendedprice where l_quantity < threshold
    int64_t sum_price = 0; // scale 2
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        int32_t pk = db->l_partkey[i];
        auto it = part_threshold.find(pk);
        if (it != part_threshold.end()) {
            if ((double)db->l_quantity[i] < it->second) {
                sum_price += db->l_extendedprice[i];
            }
        }
    }

    double avg_yearly = (sum_price / 100.0) / 7.0;

    std::ostringstream oss;
    write_csv_header(oss, {"avg_yearly"});
    write_csv_row(oss, {fmt_decimal(avg_yearly, 15)});

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
