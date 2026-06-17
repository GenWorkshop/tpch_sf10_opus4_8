#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <map>
#include <unordered_map>
#include <unordered_set>

// q9: Profit query - parts containing 'rosy' in name
// amount = l_extendedprice*(1-l_discount) - ps_supplycost*l_quantity
// Join: part, supplier, lineitem, partsupp, orders, nation
// Group by nation, o_year; order by nation ASC, o_year DESC

static void run_q9(Database* db, const std::string& run_nr) {
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

    // Find parts with p_name LIKE '%rosy%'
    std::unordered_set<int32_t> rosy_parts;
    for (int32_t pk = 1; pk < (int32_t)db->p_name.size(); pk++) {
        if (db->p_name[pk].find("rosy") != std::string::npos) {
            rosy_parts.insert(pk);
        }
    }

    // Build partsupp index: (partkey, suppkey) -> supplycost
    std::unordered_map<int64_t, int64_t> ps_cost; // key = partkey*1000000LL + suppkey
    {
        PROFILE_SCOPE("q9_partsupp_build");
        for (int64_t i = 0; i < db->partsupp_count; i++) {
            if (rosy_parts.count(db->ps_partkey[i])) {
                int64_t key = (int64_t)db->ps_partkey[i] * 1000000LL + db->ps_suppkey[i];
                ps_cost[key] = db->ps_supplycost[i];
            }
        }
    }
    TRACE_COUNT("q9_rosy_parts", rosy_parts.size());
    TRACE_COUNT("q9_ps_build_rows", ps_cost.size());

    // Build orderkey -> year map
    std::unordered_map<int32_t, int> order_to_year;
    {
        PROFILE_SCOPE("q9_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            order_to_year[db->o_orderkey[i]] = days_to_year(db->o_orderdate[i]);
        }
    }
    TRACE_COUNT("q9_orders_build_rows", order_to_year.size());

    // Group by (nation_name, year) -> sum_profit
    // nation comes from supplier's nationkey
    struct Key {
        std::string nation;
        int year;
        bool operator<(const Key& o) const {
            if (nation != o.nation) return nation < o.nation;
            return year > o.year; // DESC
        }
    };
    std::map<Key, __int128> groups;

    // Scan lineitem
    TRACE_DECL(rows_scanned);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q9_lineitem_probe_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t pk = db->l_partkey[i];
            if (!rosy_parts.count(pk)) continue;

            int32_t sk = db->l_suppkey[i];
            int64_t ps_key = (int64_t)pk * 1000000LL + sk;
            auto cost_it = ps_cost.find(ps_key);
            if (cost_it == ps_cost.end()) continue;

            int32_t ok = db->l_orderkey[i];
            auto yr_it = order_to_year.find(ok);
            if (yr_it == order_to_year.end()) continue;
            TRACE_INC(join_rows_emitted);

            // amount = l_extendedprice*(1-l_discount) - ps_supplycost*l_quantity
            // extendedprice(scale2) * (100 - discount(scale2)) = scale4
            // supplycost(scale2) * quantity(scale2) = scale4
            // amount is scale4
            int64_t revenue = db->l_extendedprice[i] * (100 - db->l_discount[i]);
            int64_t cost = cost_it->second * db->l_quantity[i];
            __int128 amount = (__int128)revenue - (__int128)cost;

            int32_t s_nk = db->s_nationkey[sk];
            Key key{db->nationkey_to_name[s_nk], yr_it->second};
            groups[key] += amount;
        }
    }
    TRACE_COUNT("q9_rows_scanned", rows_scanned);
    TRACE_COUNT("q9_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q9_groups_created", groups.size());

    {
    PROFILE_SCOPE("q9_output");
    std::ostringstream oss;
    write_csv_header(oss, {"nation","o_year","sum_profit"});
    for (auto& [key, profit] : groups) {
        double profit_d = static_cast<double>(static_cast<long long>(profit)) / 10000.0;
        write_csv_row(oss, {
            csv_quote(key.nation),
            std::to_string(key.year),
            fmt_decimal(profit_d, 4)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q9_query_output_rows", groups.size());
    }
    TRACE_FLUSH();
}
