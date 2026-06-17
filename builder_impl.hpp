#pragma once

#include "loader_impl.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

// Date stored as days since 1970-01-01 (epoch)
using Date = int32_t;

struct Nation {
    std::string n_name;
    int32_t n_regionkey;
};

struct Region {
    std::string r_name;
};

struct Database {
    // ---- REGION (5 rows) ----
    std::vector<std::string> r_name;
    int32_t n_regions;

    // ---- NATION (25 rows) ----
    std::vector<std::string> n_name;
    std::vector<int32_t> n_regionkey;
    int32_t n_nations;
    // Derived: region_name → regionkey lookup
    std::unordered_map<std::string, int32_t> region_name_to_key;
    // Derived: nation_name → nationkey lookup
    std::unordered_map<std::string, int32_t> nation_name_to_key;
    // Derived: nations per region (regionkey → list of nationkeys)
    std::vector<std::vector<int32_t>> nations_in_region; // indexed by regionkey

    // ---- SUPPLIER ----
    int32_t n_supplier;
    std::vector<std::string> s_name;
    std::vector<std::string> s_address;
    std::vector<int32_t> s_nationkey;
    std::vector<std::string> s_phone;
    std::vector<int64_t> s_acctbal;  // cents
    std::vector<std::string> s_comment;
    // Derived: suppkey → nationkey (direct array, 0-indexed by suppkey-1)

    // ---- CUSTOMER ----
    int32_t n_customer;
    std::vector<std::string> c_name;
    std::vector<std::string> c_address;
    std::vector<int32_t> c_nationkey;
    std::vector<std::string> c_phone;
    std::vector<int64_t> c_acctbal;  // cents
    std::vector<std::string> c_mktsegment;
    std::vector<std::string> c_comment;

    // ---- PART ----
    int32_t n_part;
    std::vector<std::string> p_name;
    std::vector<std::string> p_mfgr;
    std::vector<std::string> p_brand;
    std::vector<std::string> p_type;
    std::vector<int32_t> p_size;
    std::vector<std::string> p_container;
    std::vector<int64_t> p_retailprice;  // cents

    // ---- PARTSUPP ----
    int32_t n_partsupp;
    std::vector<int32_t> ps_partkey;
    std::vector<int32_t> ps_suppkey;
    std::vector<int32_t> ps_availqty;
    std::vector<int64_t> ps_supplycost;  // cents

    // ---- ORDERS ----
    int32_t n_orders;
    std::vector<int32_t> o_orderkey;
    std::vector<int32_t> o_custkey;
    std::vector<char> o_orderstatus;
    std::vector<int64_t> o_totalprice;  // cents
    std::vector<Date> o_orderdate;
    std::vector<std::string> o_orderpriority;
    std::vector<int32_t> o_shippriority;
    std::vector<std::string> o_comment;
    // Derived: orderkey → row index (direct map for dense orderkeys)
    std::vector<int32_t> orderkey_to_idx;  // indexed by orderkey
    int32_t max_orderkey;
    bool orders_sorted_by_orderkey = false;

    // ---- LINEITEM ----
    int64_t n_lineitem;
    std::vector<int32_t> l_orderkey;
    std::vector<int32_t> l_partkey;
    std::vector<int32_t> l_suppkey;
    std::vector<int32_t> l_linenumber;
    std::vector<int64_t> l_quantity;       // cents (×100)
    std::vector<int64_t> l_extendedprice;  // cents
    std::vector<int64_t> l_discount;       // cents (×100, i.e. 0.05 = 5)
    std::vector<int64_t> l_tax;            // cents (×100)
    std::vector<char> l_returnflag;
    std::vector<char> l_linestatus;
    std::vector<Date> l_shipdate;
    std::vector<Date> l_commitdate;
    std::vector<Date> l_receiptdate;
    std::vector<std::string> l_shipinstruct;
    std::vector<std::string> l_shipmode;

    // Pre-computed: l_extendedprice * (1 - l_discount) in cents×100 (i.e. ×10000 from dollars)
    // Actually stored as: extendedprice_cents * (100 - discount_pct) to avoid floating point
    // revenue[i] = l_extendedprice[i] * (100 - l_discount[i])  (units: cents * percent_complement)
    // To get dollars: revenue[i] / 10000.0
    // No, let's keep it simpler: store as int64 raw product for exact computation at query time.

    // Orderkey CSR: for each orderkey, the [start,end) range in the lineitem
    // arrays, packed so both bounds land in a single cacheline gather.
    // (only populated if lineitem is sorted by orderkey)
    struct LineitemRange { int32_t start; int32_t end; };
    std::vector<LineitemRange> orderkey_lineitem_range;  // indexed by orderkey
    bool lineitem_sorted_by_orderkey = false;
};


Database* build(ParquetTables*);
