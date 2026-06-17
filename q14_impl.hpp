#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include "trace_utils.hpp"
#include <sstream>
#include <fstream>

// q14: Promotion effect - lineitem join part
// Filter: l_shipdate in [1995-05-01, 1995-06-01)
// promo_revenue = 100 * sum(CASE WHEN p_type LIKE 'PROMO%' THEN volume ELSE 0) / sum(volume)
// volume = l_extendedprice * (1 - l_discount)

static void run_q14(Database* db, const std::string& run_nr) {
    auto to_days = [](int y, int m, int d) -> int32_t {
        y -= (m <= 2);
        int era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<int>(doe) - 719468;
    };

    const int32_t date_start = to_days(1995, 5, 1);
    const int32_t date_end = to_days(1995, 6, 1);

    int64_t promo_sum = 0;
    int64_t total_sum = 0;

    TRACE_DECL(rows_scanned);
    TRACE_DECL(rows_emitted);
    {
        PROFILE_SCOPE("q14_scan_join");
        for (int64_t i = 0; i < db->lineitem_count; i++) {
            TRACE_INC(rows_scanned);
            int32_t sd = db->l_shipdate[i];
            if (sd >= date_start && sd < date_end) {
                TRACE_INC(rows_emitted);
                int64_t volume = db->l_extendedprice[i] * (100 - db->l_discount[i]);
                total_sum += volume;

                int32_t pk = db->l_partkey[i];
                const std::string& ptype = db->p_type[pk];
                if (ptype.size() >= 5 && ptype.substr(0, 5) == "PROMO") {
                    promo_sum += volume;
                }
            }
        }
    }
    TRACE_COUNT("q14_rows_scanned", rows_scanned);
    TRACE_COUNT("q14_rows_emitted", rows_emitted);
    TRACE_COUNT("q14_probe_rows_in", rows_emitted);
    TRACE_COUNT("q14_join_rows_emitted", rows_emitted);

    double promo_revenue = total_sum > 0 ? 100.0 * (double)promo_sum / (double)total_sum : 0.0;

    {
    PROFILE_SCOPE("q14_output");
    std::ostringstream oss;
    write_csv_header(oss, {"promo_revenue"});
    write_csv_row(oss, {fmt_decimal(promo_revenue, 15)});

    std::string result = oss.str();
    std::ofstream out("result" + run_nr + ".csv");
    out << result;
    out.close();
    std::cout << result;
    TRACE_COUNT("q14_query_output_rows", 1);
    }
    TRACE_FLUSH();
}
