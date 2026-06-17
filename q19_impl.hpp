#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <sstream>
#include <fstream>
#include <unordered_set>

// q19: Discounted revenue - lineitem join part with 3 OR conditions
// Each condition: specific brand, container set, quantity range, size range,
// shipmode IN ('AIR','AIR REG'), shipinstruct = 'DELIVER IN PERSON'
// revenue = sum(l_extendedprice * (1 - l_discount)) scale 4

static void run_q19(Database* db, const std::string& run_nr) {
    int64_t revenue = 0; // scale 4

    for (int64_t i = 0; i < db->lineitem_count; i++) {
        // Common filters first
        const std::string& mode = db->l_shipmode[i];
        if (mode != "AIR" && mode != "AIR REG") continue;
        if (db->l_shipinstruct[i] != "DELIVER IN PERSON") continue;

        int32_t pk = db->l_partkey[i];
        int64_t qty = db->l_quantity[i]; // scale 2
        const std::string& brand = db->p_brand[pk];
        const std::string& container = db->p_container[pk];
        int32_t size = db->p_size[pk];

        bool match = false;

        // Condition 1: Brand#14, SM containers, qty 1-11, size 1-5
        if (brand == "Brand#14" &&
            (container == "SM CASE" || container == "SM BOX" || container == "SM PACK" || container == "SM PKG") &&
            qty >= 100 && qty <= 1100 &&  // scale 2: 1*100=100, 11*100=1100
            size >= 1 && size <= 5) {
            match = true;
        }

        // Condition 2: Brand#15, MED containers, qty 17-27, size 1-10
        if (!match && brand == "Brand#15" &&
            (container == "MED BAG" || container == "MED BOX" || container == "MED PKG" || container == "MED PACK") &&
            qty >= 1700 && qty <= 2700 &&
            size >= 1 && size <= 10) {
            match = true;
        }

        // Condition 3: Brand#35, LG containers, qty 28-38, size 1-15
        if (!match && brand == "Brand#35" &&
            (container == "LG CASE" || container == "LG BOX" || container == "LG PACK" || container == "LG PKG") &&
            qty >= 2800 && qty <= 3800 &&
            size >= 1 && size <= 15) {
            match = true;
        }

        if (match) {
            revenue += db->l_extendedprice[i] * (100 - db->l_discount[i]);
        }
    }

    std::ostringstream oss;
    write_csv_header(oss, {"revenue"});
    write_csv_row(oss, {fmt_decimal(revenue / 10000.0, 4)});

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
