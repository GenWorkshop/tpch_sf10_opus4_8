#pragma once

#include "loader_impl.hpp"
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

// Date epoch: 1970-01-01 (Arrow's default for DATE32)
// We store dates as int32 days since epoch.

struct Database {
    // ========== REGION (5 rows) ==========
    std::vector<int32_t> r_regionkey;
    std::vector<std::string> r_name;

    // ========== NATION (25 rows) ==========
    std::vector<int32_t> n_nationkey;
    std::vector<std::string> n_name;
    std::vector<int32_t> n_regionkey;

    // Lookup helpers
    std::unordered_map<std::string, int32_t> region_name_to_key;
    std::vector<std::vector<int32_t>> region_to_nation_keys; // regionkey -> list of nationkeys
    std::vector<int32_t> nation_to_region; // nationkey -> regionkey (size 25)
    std::vector<std::string> nationkey_to_name; // nationkey -> name

    // ========== SUPPLIER (indexed by suppkey, 1-based) ==========
    int32_t supplier_count;
    std::vector<std::string> s_name;
    std::vector<std::string> s_address;
    std::vector<int32_t> s_nationkey;
    std::vector<std::string> s_phone;
    std::vector<int64_t> s_acctbal;  // cents
    std::vector<std::string> s_comment;

    // Auxiliary: nation-grouped supplier lists
    std::vector<std::vector<int32_t>> nation_to_suppliers; // nationkey -> list of suppkeys
    // Precomputed complaint suppliers (q16)
    std::unordered_set<int32_t> complaint_suppliers;

    // ========== CUSTOMER (indexed by custkey, 1-based) ==========
    int32_t customer_count;
    std::vector<std::string> c_name;
    std::vector<std::string> c_address;
    std::vector<int32_t> c_nationkey;
    std::vector<std::string> c_phone;
    std::vector<int64_t> c_acctbal;  // cents
    std::vector<std::string> c_mktsegment;
    std::vector<std::string> c_comment;

    // ========== PART (indexed by partkey, 1-based) ==========
    int32_t part_count;
    std::vector<std::string> p_name;
    std::vector<std::string> p_mfgr;
    std::vector<std::string> p_brand;
    std::vector<std::string> p_type;
    std::vector<int32_t> p_size;
    std::vector<std::string> p_container;

    // ========== PARTSUPP ==========
    int64_t partsupp_count;
    std::vector<int32_t> ps_partkey;
    std::vector<int32_t> ps_suppkey;
    std::vector<int32_t> ps_availqty;
    std::vector<int64_t> ps_supplycost; // cents
    // Index: partkey -> start index in partsupp arrays (sorted by partkey)
    // Each part has exactly 4 suppliers
    // We'll store them sorted by partkey for direct access

    // ========== ORDERS (indexed by position) ==========
    int64_t orders_count;
    std::vector<int32_t> o_orderkey;
    std::vector<int32_t> o_custkey;
    std::vector<char> o_orderstatus;
    std::vector<int64_t> o_totalprice; // cents
    std::vector<int32_t> o_orderdate;  // days since epoch
    std::vector<std::string> o_orderpriority;
    std::vector<int32_t> o_shippriority;
    std::vector<std::string> o_comment;

    // orderkey -> index in orders arrays (for O(1) lookup)
    std::vector<int32_t> orderkey_to_idx; // indexed by orderkey, -1 if not present

    // custkey -> list of order indices (for q13, q22)
    std::vector<std::vector<int32_t>> custkey_to_order_idxs;

    // ========== LINEITEM ==========
    int64_t lineitem_count;
    std::vector<int32_t> l_orderkey;
    std::vector<int32_t> l_partkey;
    std::vector<int32_t> l_suppkey;
    std::vector<int32_t> l_linenumber;
    std::vector<int64_t> l_quantity;       // stored as cents (DECIMAL*100)
    std::vector<int64_t> l_extendedprice;  // cents
    std::vector<int64_t> l_discount;       // cents (0.01 = 1)
    std::vector<int64_t> l_tax;            // cents
    std::vector<char> l_returnflag;
    std::vector<char> l_linestatus;
    std::vector<int32_t> l_shipdate;       // days since epoch
    std::vector<int32_t> l_commitdate;
    std::vector<int32_t> l_receiptdate;
    std::vector<std::string> l_shipinstruct;
    std::vector<std::string> l_shipmode;

    // Auxiliary: orderkey -> range of lineitem indices
    // (lineitems sorted by orderkey for fast lookup)
    std::vector<int32_t> orderkey_to_li_start; // indexed by orderkey
    std::vector<int32_t> orderkey_to_li_end;
    int32_t max_orderkey;
};


Database* build(ParquetTables*);
