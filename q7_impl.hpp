#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <map>
#include <unordered_map>

// q7: shipping query - ALGERIA/BRAZIL nation pairs, shipdate 1995-1996
// Join supplier->lineitem->orders->customer, with nation lookups
// Group by supp_nation, cust_nation, l_year; sum volume (scale 4)
// Order by supp_nation, cust_nation, l_year

static void run_q7(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    // Extract year from days since epoch
    auto days_to_year = [](int32_t days) -> int {
        days += 719468;
        int era = (days >= 0 ? days : days - 146096) / 146097;
        unsigned doe = static_cast<unsigned>(days - era * 146097);
        unsigned yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
        int y = static_cast<int>(yoe) + era * 400;
        unsigned doy = doe - (365*yoe + yoe/4 - yoe/100);
        unsigned mp = (5*doy + 2) / 153;
        unsigned m = mp + (mp < 10 ? 3 : -9);
        y += (m <= 2);
        return y;
    };

    const int32_t date_start = to_days(1995, 1, 1);
    const int32_t date_end = to_days(1996, 12, 31);

    // Find ALGERIA and BRAZIL nation keys
    int32_t algeria_nk = -1, brazil_nk = -1;
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        if (db->n_name[i] == "ALGERIA") algeria_nk = db->n_nationkey[i];
        if (db->n_name[i] == "BRAZIL") brazil_nk = db->n_nationkey[i];
    }

    // Build orderkey -> customer nationkey map
    std::unordered_map<int32_t, int32_t> order_to_cust_nk;
    for (int64_t i = 0; i < db->orders_count; i++) {
        int32_t ck = db->o_custkey[i];
        int32_t cnk = db->c_nationkey[ck];
        if (cnk == algeria_nk || cnk == brazil_nk) {
            order_to_cust_nk[db->o_orderkey[i]] = cnk;
        }
    }

    // Group key: (supp_nation_name, cust_nation_name, year)
    struct Key {
        std::string supp_nation;
        std::string cust_nation;
        int year;
        bool operator<(const Key& o) const {
            if (supp_nation != o.supp_nation) return supp_nation < o.supp_nation;
            if (cust_nation != o.cust_nation) return cust_nation < o.cust_nation;
            return year < o.year;
        }
    };
    std::map<Key, int64_t> groups;

    // Scan lineitem
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        int32_t sd = db->l_shipdate[i];
        if (sd < date_start || sd > date_end) continue;

        int32_t sk = db->l_suppkey[i];
        int32_t s_nk = db->s_nationkey[sk];
        if (s_nk != algeria_nk && s_nk != brazil_nk) continue;

        int32_t ok = db->l_orderkey[i];
        auto it = order_to_cust_nk.find(ok);
        if (it == order_to_cust_nk.end()) continue;
        int32_t c_nk = it->second;

        // Check valid pair: (ALGERIA,BRAZIL) or (BRAZIL,ALGERIA)
        if (!((s_nk == algeria_nk && c_nk == brazil_nk) ||
              (s_nk == brazil_nk && c_nk == algeria_nk))) continue;

        int year = days_to_year(sd);
        int64_t volume = db->l_extendedprice[i] * (100 - db->l_discount[i]);

        Key key{db->nationkey_to_name[s_nk], db->nationkey_to_name[c_nk], year};
        groups[key] += volume;
    }

    std::ostringstream oss;
    write_csv_header(oss, {"supp_nation","cust_nation","l_year","revenue"});
    for (auto& [key, rev] : groups) {
        write_csv_row(oss, {
            csv_quote(key.supp_nation),
            csv_quote(key.cust_nation),
            std::to_string(key.year),
            fmt_decimal(rev / 10000.0, 4)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
}
