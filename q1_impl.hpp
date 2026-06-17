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
    // l_shipdate <= '1998-12-01' - 100 days = '1998-08-23'
    const Date cutoff = date_to_epoch(1998, 8, 23);

    // Group by (returnflag, linestatus) - only a few combinations
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

    // Key: (returnflag, linestatus)
    std::map<std::pair<char,char>, Agg> groups;

    for (int64_t i = 0; i < db->n_lineitem; i++) {
        if (db->l_shipdate[i] <= cutoff) {
            auto key = std::make_pair(db->l_returnflag[i], db->l_linestatus[i]);
            auto& g = groups[key];

            int64_t qty = db->l_quantity[i];       // scale 2
            int64_t price = db->l_extendedprice[i]; // scale 2
            int64_t disc = db->l_discount[i];       // scale 2 (e.g., 0.05 = 5)
            int64_t tax = db->l_tax[i];             // scale 2

            g.sum_qty += qty;
            g.sum_base_price += price;

            // disc_price = price * (1 - discount) = price * (100 - disc) / 100
            // But to keep scale 4: price(s2) * (100 - disc)(s2 implicit as integer but disc is s2)
            // Actually: price is scale 2, discount is scale 2 (0.05 stored as 5)
            // disc_price = price * (100 - disc) → this gives scale 2 * unitless? No.
            // Let me think: l_discount stored as scale 2, so 0.05 = 5.
            // (1 - l_discount) in real = (100 - 5)/100 = 0.95
            // disc_price = l_extendedprice * (1 - l_discount)
            //   = price(s2) * (100 - disc) / 100
            // To get result in scale 4: price(s2) * (100 - disc)(s2) = scale 4
            // That's: price * (100 - disc) directly gives scale 4!
            __int128 disc_price = (__int128)price * (100 - disc);
            g.sum_disc_price += disc_price;

            // charge = disc_price * (1 + tax)
            //   = disc_price(s4) * (100 + tax)(s2) / 100? No.
            // disc_price is scale 4, tax is scale 2
            // charge = disc_price * (100 + tax) → scale 6
            __int128 charge = disc_price * (100 + tax);
            g.sum_charge += charge;

            g.sum_qty_d += qty;
            g.sum_price_d += price;
            g.sum_disc_d += disc;
            g.count++;
        }
    }

    write_csv_header(out, {"l_returnflag","l_linestatus","sum_qty","sum_base_price",
                           "sum_disc_price","sum_charge","avg_qty","avg_price","avg_disc","count_order"});

    for (auto& [key, g] : groups) {
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

        std::string rf(1, key.first);
        std::string ls(1, key.second);

        write_csv_row(out, {
            rf,
            ls,
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
}
