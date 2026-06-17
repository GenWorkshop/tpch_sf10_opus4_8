#pragma once
#include "builder_impl.hpp"
#include "query_utils.hpp"
#include <ostream>
#include <algorithm>
#include <map>

// Convert yyyy-mm-dd to days since epoch (1970-01-01)
inline int32_t date_to_epoch(int y, int m, int d) {
    // Using the algorithm from Howard Hinnant
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = static_cast<unsigned>(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

inline void run_q1_impl(Database* db, std::ostream& out) {
    PROFILE_SCOPE("q1_total");
    // l_shipdate <= '1998-12-01' - 100 days = '1998-08-23'
    const Date cutoff = date_to_epoch(1998, 8, 23);

    // Group by (returnflag, linestatus). DuckDB uses a PERFECT_HASH_GROUP_BY
    // here: both grouping columns are single uppercase chars, so a direct
    // flat array indexed by (rf-'A')*26 + (ls-'A') is a perfect hash and
    // avoids the per-row tree lookup of std::map. Iterating the array in
    // index order yields the required ORDER BY rf, ls.
    struct Agg {
        __int128 sum_qty = 0;        // scale 2
        __int128 sum_base_price = 0; // scale 2
        __int128 sum_disc_price = 0; // scale 4 (price*discount both scale 2 → product scale 4)
        __int128 sum_charge = 0;     // scale 6 (disc_price * tax, scale 4 * scale 2 → scale 6)
        double sum_qty_d = 0;
        double sum_price_d = 0;
        double sum_disc_d = 0;
        int64_t count = 0;
    };

    constexpr int N_SLOTS = 26 * 26;
    static thread_local Agg slots[N_SLOTS];
    for (int s = 0; s < N_SLOTS; s++) slots[s] = Agg{};

    const Date*    __restrict ship  = db->l_shipdate.data();
    const char*    __restrict rf    = db->l_returnflag.data();
    const char*    __restrict ls    = db->l_linestatus.data();
    const int64_t* __restrict qtyA  = db->l_quantity.data();
    const int64_t* __restrict prA   = db->l_extendedprice.data();
    const int64_t* __restrict dscA  = db->l_discount.data();
    const int64_t* __restrict taxA  = db->l_tax.data();

    TRACE_DECL_COUNTER(rows_scanned);
    TRACE_DECL_COUNTER(rows_emitted);
    {
        PROFILE_SCOPE("q1_scan_agg");
        const int64_t n = db->n_lineitem;
        for (int64_t i = 0; i < n; i++) {
            TRACE_INC(rows_scanned);
            if (ship[i] <= cutoff) {
                TRACE_INC(rows_emitted);
                int idx = (rf[i] - 'A') * 26 + (ls[i] - 'A');
                Agg& g = slots[idx];

                int64_t qty = qtyA[i];     // scale 2
                int64_t price = prA[i];    // scale 2
                int64_t disc = dscA[i];    // scale 2 (e.g., 0.05 = 5)
                int64_t tax = taxA[i];     // scale 2

                g.sum_qty += qty;
                g.sum_base_price += price;

                __int128 disc_price = (__int128)price * (100 - disc);
                g.sum_disc_price += disc_price;

                __int128 charge = disc_price * (100 + tax);
                g.sum_charge += charge;

                g.sum_qty_d += qty;
                g.sum_price_d += price;
                g.sum_disc_d += disc;
                g.count++;
            }
        }
    }

    TRACE_COUNT("q1_rows_scanned", rows_scanned);
    TRACE_COUNT("q1_rows_emitted", rows_emitted);
    TRACE_COUNT("q1_agg_rows_in", rows_emitted);

    PROFILE_SCOPE("q1_output");
    write_csv_header(out, {"l_returnflag","l_linestatus","sum_qty","sum_base_price",
                           "sum_disc_price","sum_charge","avg_qty","avg_price","avg_disc","count_order"});

    uint64_t n_groups = 0;
    for (int idx = 0; idx < N_SLOTS; idx++) {
        const Agg& g = slots[idx];
        if (g.count == 0) continue;
        n_groups++;
        std::pair<char,char> key{ char('A' + idx / 26), char('A' + idx % 26) };
        // sum_qty: scale 2
        std::string s_sum_qty = fmt_money(static_cast<long long>(g.sum_qty), 2);
        // sum_base_price: scale 2
        std::string s_sum_base_price = fmt_money(static_cast<long long>(g.sum_base_price), 2);
        // sum_disc_price: scale 4
        std::string s_sum_disc_price = fmt_money(static_cast<long long>(g.sum_disc_price), 4);
        // sum_charge: scale 6
        std::string s_sum_charge = fmt_money(static_cast<long long>(g.sum_charge), 6);
        // avg_qty: qty is scale 2, so avg = sum_qty_d / count / 100
        double avg_qty = (g.sum_qty_d / g.count) / 100.0;
        // avg_price: price is scale 2
        double avg_price = (g.sum_price_d / g.count) / 100.0;
        // avg_disc: disc is scale 2
        double avg_disc = (g.sum_disc_d / g.count) / 100.0;

        std::string rf_s(1, key.first);
        std::string ls_s(1, key.second);

        write_csv_row(out, {
            rf_s,
            ls_s,
            s_sum_qty,
            s_sum_base_price,
            s_sum_disc_price,
            s_sum_charge,
            fmt_decimal(avg_qty, 15),
            fmt_decimal(avg_price, 15),
            fmt_decimal(avg_disc, 15),
            std::to_string(g.count)
        });
    }
    TRACE_COUNT("q1_groups_created", n_groups);
    TRACE_COUNT("q1_agg_rows_emitted", n_groups);
    TRACE_COUNT("q1_query_output_rows", n_groups);
}
