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

// q8: Market share - part(type='ECONOMY BRUSHED TIN') join lineitem join orders(date 1995-1996)
// join customer join nation(AMERICA region) join supplier(get n2.n_name)
// mkt_share = sum(CASE WHEN nation='FRANCE' THEN volume ELSE 0) / sum(volume)
// Group by o_year, order by o_year

static void run_q8(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

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

    // Find AMERICA region nations (for customer filter)
    int32_t america_rk = db->region_name_to_key["AMERICA"];
    std::unordered_set<int32_t> america_nations;
    for (int32_t nk : db->region_to_nation_keys[america_rk]) {
        america_nations.insert(nk);
    }

    // Find FRANCE nationkey (for supplier nation check)
    int32_t france_nk = -1;
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        if (db->n_name[i] == "FRANCE") { france_nk = db->n_nationkey[i]; break; }
    }

    // Find parts with p_type = 'ECONOMY BRUSHED TIN'
    std::unordered_set<int32_t> matching_parts;
    for (int32_t pk = 1; pk < (int32_t)db->p_type.size(); pk++) {
        if (db->p_type[pk] == "ECONOMY BRUSHED TIN") {
            matching_parts.insert(pk);
        }
    }

    // Qualifying orders: orderdate in [1995-01-01, 1996-12-31], customer in AMERICA
    // Map orderkey -> (year)
    std::unordered_map<int32_t, int> order_to_year;
    TRACE_DECL(orders_scanned);
    {
        PROFILE_SCOPE("q8_orders_build");
        for (int64_t i = 0; i < db->orders_count; i++) {
            TRACE_INC(orders_scanned);
            int32_t od = db->o_orderdate[i];
            if (od >= date_start && od <= date_end) {
                int32_t ck = db->o_custkey[i];
                int32_t cnk = db->c_nationkey[ck];
                if (america_nations.count(cnk)) {
                    order_to_year[db->o_orderkey[i]] = days_to_year(od);
                }
            }
        }
    }
    TRACE_COUNT("q8_orders_scanned", orders_scanned);
    TRACE_COUNT("q8_build_rows", order_to_year.size());

    // Scan lineitem: partkey in matching_parts, orderkey in qualifying orders
    // volume = extendedprice * (1 - discount) (scale 4)
    // Group by year: sum_france_volume, sum_total_volume
    struct YearAgg { int64_t france_vol = 0; int64_t total_vol = 0; };
    std::map<int, YearAgg> groups;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(join_rows_emitted);
    {
        PROFILE_SCOPE("q8_lineitem_probe_agg");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            if (!matching_parts.count(db->l_partkey[i])) continue;
            int32_t ok = db->l_orderkey[i];
            auto it = order_to_year.find(ok);
            if (it == order_to_year.end()) continue;
            TRACE_INC(join_rows_emitted);

            int64_t volume = db->l_extendedprice[i] * (100 - db->l_discount[i]);
            int year = it->second;
            groups[year].total_vol += volume;

            // Check if supplier nation is FRANCE
            int32_t sk = db->l_suppkey[i];
            if (db->s_nationkey[sk] == france_nk) {
                groups[year].france_vol += volume;
            }
        }
    }
    TRACE_COUNT("q8_rows_scanned", rows_scanned);
    TRACE_COUNT("q8_join_rows_emitted", join_rows_emitted);
    TRACE_COUNT("q8_groups_created", groups.size());

    {
    PROFILE_SCOPE("q8_output");
    std::ostringstream oss;
    write_csv_header(oss, {"o_year","mkt_share"});
    for (auto& [year, agg] : groups) {
        double mkt_share = agg.total_vol > 0 ? 
            static_cast<double>(agg.france_vol) / static_cast<double>(agg.total_vol) : 0.0;
        write_csv_row(oss, {
            std::to_string(year),
            fmt_decimal(mkt_share, 15)
        });
    }

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q8_query_output_rows", groups.size());
    }
    TRACE_FLUSH();
}
