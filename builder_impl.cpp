#include "builder_impl.hpp"
#include <arrow/api.h>
#include <arrow/array.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <numeric>
#include <thread>
#include <functional>
#include <vector>

// Helper to get int32 column (handles both int32 and int64 source types)
static std::vector<int32_t> get_int32_col(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<int32_t> result(n);
    int64_t pos = 0;
    auto type_id = chunked->type()->id();
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto chunk = chunked->chunk(c);
        int64_t len = chunk->length();
        if (type_id == arrow::Type::INT64) {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            const int64_t* raw = arr->raw_values();
            for (int64_t i = 0; i < len; i++) result[pos++] = static_cast<int32_t>(raw[i]);
        } else {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            const int32_t* raw = arr->raw_values();
            memcpy(&result[pos], raw, len * sizeof(int32_t));
            pos += len;
        }
    }
    return result;
}

// Helper to get int64 column (handles both int32 and int64 source types)
static std::vector<int64_t> get_int64_col(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<int64_t> result(n);
    int64_t pos = 0;
    auto type_id = chunked->type()->id();
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto chunk = chunked->chunk(c);
        int64_t len = chunk->length();
        if (type_id == arrow::Type::INT32) {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            const int32_t* raw = arr->raw_values();
            for (int64_t i = 0; i < len; i++) result[pos++] = static_cast<int64_t>(raw[i]);
        } else {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            const int64_t* raw = arr->raw_values();
            memcpy(&result[pos], raw, len * sizeof(int64_t));
            pos += len;
        }
    }
    return result;
}

// Helper to get string column
static std::vector<std::string> get_string_col(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<std::string> result(n);
    int64_t pos = 0;
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(chunked->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            auto sv = arr->GetView(i);
            result[pos++].assign(sv.data(), sv.size());
        }
    }
    return result;
}

// Helper to get date32 column (days since epoch)
static std::vector<int32_t> get_date32_col(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<int32_t> result(n);
    int64_t pos = 0;
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Date32Array>(chunked->chunk(c));
        const int32_t* raw = arr->raw_values();
        int64_t len = arr->length();
        memcpy(&result[pos], raw, len * sizeof(int32_t));
        pos += len;
    }
    return result;
}

// Helper to get decimal128 column as int64
static std::vector<int64_t> get_decimal_col_v2(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<int64_t> result(n);
    int64_t pos = 0;
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Decimal128Array>(chunked->chunk(c));
        // Decimal128 is 16 bytes per value, stored contiguously
        const uint8_t* raw_data = arr->values()->data() + arr->offset() * 16;
        for (int64_t i = 0; i < arr->length(); i++) {
            // For TPC-H values, the low 8 bytes suffice (values fit in int64)
            int64_t lo;
            memcpy(&lo, raw_data + i * 16, 8);
            int64_t hi_byte;
            memcpy(&hi_byte, raw_data + i * 16 + 8, 8);
            // If high bits are -1 (sign extension of negative), or 0 (positive), lo is correct
            result[pos++] = lo;
        }
    }
    return result;
}

// Helper to get single-char column
static std::vector<char> get_char_col(const std::shared_ptr<arrow::Table>& table, int col_idx) {
    auto chunked = table->column(col_idx);
    int64_t n = table->num_rows();
    std::vector<char> result(n);
    int64_t pos = 0;
    for (int c = 0; c < chunked->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(chunked->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            auto sv = arr->GetView(i);
            result[pos++] = sv.length() > 0 ? sv[0] : ' ';
        }
    }
    return result;
}

Database* build(ParquetTables* pt) {
    auto db = new Database{};

    // ===== REGION & NATION (tiny, do first sequentially) =====
    db->r_regionkey = get_int32_col(pt->region, 0);
    db->r_name = get_string_col(pt->region, 1);
    for (size_t i = 0; i < db->r_regionkey.size(); i++) {
        db->region_name_to_key[db->r_name[i]] = db->r_regionkey[i];
    }

    db->n_nationkey = get_int32_col(pt->nation, 0);
    db->n_name = get_string_col(pt->nation, 1);
    db->n_regionkey = get_int32_col(pt->nation, 2);

    db->nation_to_region.resize(25, -1);
    db->nationkey_to_name.resize(25);
    db->region_to_nation_keys.resize(5);
    for (size_t i = 0; i < db->n_nationkey.size(); i++) {
        int nk = db->n_nationkey[i];
        int rk = db->n_regionkey[i];
        db->nation_to_region[nk] = rk;
        db->nationkey_to_name[nk] = db->n_name[i];
        db->region_to_nation_keys[rk].push_back(nk);
    }

    // ===== Parallel extraction of large tables =====
    // Thread 1: Supplier
    // Thread 2: Customer
    // Thread 3: Part + Partsupp
    // Thread 4: Orders
    // Thread 5+: Lineitem columns (the biggest table)

    // Pre-read keys to determine sizes for indexed arrays
    auto s_suppkeys = get_int32_col(pt->supplier, 0);
    auto c_custkeys = get_int32_col(pt->customer, 0);
    auto p_partkeys = get_int32_col(pt->part, 0);

    int32_t max_suppkey = *std::max_element(s_suppkeys.begin(), s_suppkeys.end());
    int32_t max_custkey = *std::max_element(c_custkeys.begin(), c_custkeys.end());
    int32_t max_partkey = *std::max_element(p_partkeys.begin(), p_partkeys.end());

    db->supplier_count = pt->supplier->num_rows();
    db->customer_count = pt->customer->num_rows();
    db->part_count = pt->part->num_rows();
    db->partsupp_count = pt->partsupp->num_rows();
    db->orders_count = pt->orders->num_rows();
    db->lineitem_count = pt->lineitem->num_rows();

    // Pre-allocate indexed arrays
    db->s_name.resize(max_suppkey + 1);
    db->s_address.resize(max_suppkey + 1);
    db->s_nationkey.resize(max_suppkey + 1, 0);
    db->s_phone.resize(max_suppkey + 1);
    db->s_acctbal.resize(max_suppkey + 1, 0);
    db->s_comment.resize(max_suppkey + 1);
    db->nation_to_suppliers.resize(25);

    db->c_name.resize(max_custkey + 1);
    db->c_address.resize(max_custkey + 1);
    db->c_nationkey.resize(max_custkey + 1, 0);
    db->c_phone.resize(max_custkey + 1);
    db->c_acctbal.resize(max_custkey + 1, 0);
    db->c_mktsegment.resize(max_custkey + 1);
    db->c_comment.resize(max_custkey + 1);

    db->p_name.resize(max_partkey + 1);
    db->p_mfgr.resize(max_partkey + 1);
    db->p_brand.resize(max_partkey + 1);
    db->p_type.resize(max_partkey + 1);
    db->p_size.resize(max_partkey + 1, 0);
    db->p_container.resize(max_partkey + 1);

    // Launch threads for large table extraction
    std::thread t_supplier([&]() {
        auto s_names = get_string_col(pt->supplier, 1);
        auto s_addresses = get_string_col(pt->supplier, 2);
        auto s_nationkeys = get_int32_col(pt->supplier, 3);
        auto s_phones = get_string_col(pt->supplier, 4);
        auto s_acctbals = get_decimal_col_v2(pt->supplier, 5);
        auto s_comments = get_string_col(pt->supplier, 6);

        for (int64_t i = 0; i < (int64_t)s_suppkeys.size(); i++) {
            int32_t sk = s_suppkeys[i];
            db->s_name[sk] = std::move(s_names[i]);
            db->s_address[sk] = std::move(s_addresses[i]);
            db->s_nationkey[sk] = s_nationkeys[i];
            db->s_phone[sk] = std::move(s_phones[i]);
            db->s_acctbal[sk] = s_acctbals[i];
            db->s_comment[sk] = std::move(s_comments[i]);
            db->nation_to_suppliers[s_nationkeys[i]].push_back(sk);
        }

        // Precompute complaint suppliers
        for (int64_t i = 0; i < (int64_t)s_suppkeys.size(); i++) {
            int32_t sk = s_suppkeys[i];
            const auto& comment = db->s_comment[sk];
            auto pos1 = comment.find("Customer");
            if (pos1 != std::string::npos) {
                auto pos2 = comment.find("Complaints", pos1 + 8);
                if (pos2 != std::string::npos) {
                    db->complaint_suppliers.insert(sk);
                }
            }
        }
    });

    std::thread t_customer([&]() {
        auto c_names = get_string_col(pt->customer, 1);
        auto c_addresses = get_string_col(pt->customer, 2);
        auto c_nationkeys = get_int32_col(pt->customer, 3);
        auto c_phones = get_string_col(pt->customer, 4);
        auto c_acctbals = get_decimal_col_v2(pt->customer, 5);
        auto c_mktsegments = get_string_col(pt->customer, 6);
        auto c_comments = get_string_col(pt->customer, 7);

        for (int64_t i = 0; i < (int64_t)c_custkeys.size(); i++) {
            int32_t ck = c_custkeys[i];
            db->c_name[ck] = std::move(c_names[i]);
            db->c_address[ck] = std::move(c_addresses[i]);
            db->c_nationkey[ck] = c_nationkeys[i];
            db->c_phone[ck] = std::move(c_phones[i]);
            db->c_acctbal[ck] = c_acctbals[i];
            db->c_mktsegment[ck] = std::move(c_mktsegments[i]);
            db->c_comment[ck] = std::move(c_comments[i]);
        }
    });

    std::thread t_part([&]() {
        auto p_names = get_string_col(pt->part, 1);
        auto p_mfgrs = get_string_col(pt->part, 2);
        auto p_brands = get_string_col(pt->part, 3);
        auto p_types = get_string_col(pt->part, 4);
        auto p_sizes = get_int32_col(pt->part, 5);
        auto p_containers = get_string_col(pt->part, 6);

        for (int64_t i = 0; i < (int64_t)p_partkeys.size(); i++) {
            int32_t pk = p_partkeys[i];
            db->p_name[pk] = std::move(p_names[i]);
            db->p_mfgr[pk] = std::move(p_mfgrs[i]);
            db->p_brand[pk] = std::move(p_brands[i]);
            db->p_type[pk] = std::move(p_types[i]);
            db->p_size[pk] = p_sizes[i];
            db->p_container[pk] = std::move(p_containers[i]);
        }
    });

    std::thread t_partsupp([&]() {
        db->ps_partkey = get_int32_col(pt->partsupp, 0);
        db->ps_suppkey = get_int32_col(pt->partsupp, 1);
        db->ps_availqty = get_int32_col(pt->partsupp, 2);
        db->ps_supplycost = get_decimal_col_v2(pt->partsupp, 3);
    });

    std::thread t_orders([&]() {
        db->o_orderkey = get_int32_col(pt->orders, 0);
        db->o_custkey = get_int32_col(pt->orders, 1);
        db->o_orderstatus = get_char_col(pt->orders, 2);
        db->o_totalprice = get_decimal_col_v2(pt->orders, 3);
        db->o_orderdate = get_date32_col(pt->orders, 4);
        db->o_orderpriority = get_string_col(pt->orders, 5);
        db->o_shippriority = get_int32_col(pt->orders, 7);
        db->o_comment = get_string_col(pt->orders, 8);
    });

    // Lineitem: extract all columns in parallel threads
    std::thread t_li_keys([&]() {
        db->l_orderkey = get_int32_col(pt->lineitem, 0);
        db->l_partkey = get_int32_col(pt->lineitem, 1);
        db->l_suppkey = get_int32_col(pt->lineitem, 2);
        db->l_linenumber = get_int32_col(pt->lineitem, 3);
    });

    std::thread t_li_nums([&]() {
        db->l_quantity = get_decimal_col_v2(pt->lineitem, 4);
        db->l_extendedprice = get_decimal_col_v2(pt->lineitem, 5);
        db->l_discount = get_decimal_col_v2(pt->lineitem, 6);
        db->l_tax = get_decimal_col_v2(pt->lineitem, 7);
    });

    std::thread t_li_flags([&]() {
        db->l_returnflag = get_char_col(pt->lineitem, 8);
        db->l_linestatus = get_char_col(pt->lineitem, 9);
        db->l_shipdate = get_date32_col(pt->lineitem, 10);
        db->l_commitdate = get_date32_col(pt->lineitem, 11);
        db->l_receiptdate = get_date32_col(pt->lineitem, 12);
    });

    std::thread t_li_str([&]() {
        db->l_shipinstruct = get_string_col(pt->lineitem, 13);
        db->l_shipmode = get_string_col(pt->lineitem, 14);
    });

    // Wait for all extraction to complete
    t_supplier.join();
    t_customer.join();
    t_part.join();
    t_partsupp.join();
    t_orders.join();
    t_li_keys.join();
    t_li_nums.join();
    t_li_flags.join();
    t_li_str.join();

    // ===== Build indexes (can parallelize some) =====

    // Orders: orderkey -> index map
    int32_t max_ok = *std::max_element(db->o_orderkey.begin(), db->o_orderkey.end());
    
    std::thread t_order_idx([&]() {
        db->orderkey_to_idx.assign(max_ok + 1, -1);
        for (int64_t i = 0; i < db->orders_count; i++) {
            db->orderkey_to_idx[db->o_orderkey[i]] = i;
        }
    });

    std::thread t_cust_order_idx([&]() {
        db->custkey_to_order_idxs.resize(max_custkey + 1);
        for (int64_t i = 0; i < db->orders_count; i++) {
            int32_t ck = db->o_custkey[i];
            if (ck <= max_custkey) {
                db->custkey_to_order_idxs[ck].push_back(i);
            }
        }
    });

    t_order_idx.join();
    t_cust_order_idx.join();

    // ===== Lineitem: sort by orderkey using counting sort (radix) =====
    // This is much faster than std::sort for integer keys
    db->max_orderkey = max_ok;
    
    // Count lineitems per orderkey
    std::vector<int32_t> li_counts(max_ok + 2, 0);
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        li_counts[db->l_orderkey[i]]++;
    }

    // Compute prefix sums
    db->orderkey_to_li_start.resize(max_ok + 2, 0);
    db->orderkey_to_li_end.resize(max_ok + 2, 0);
    std::vector<int32_t> write_pos(max_ok + 2, 0);
    int32_t running = 0;
    for (int32_t ok = 0; ok <= max_ok; ok++) {
        db->orderkey_to_li_start[ok] = running;
        write_pos[ok] = running;
        running += li_counts[ok];
        db->orderkey_to_li_end[ok] = running;
    }

    // Build permutation using counting sort (O(n) instead of O(n log n))
    std::vector<int32_t> perm(db->lineitem_count);
    for (int64_t i = 0; i < db->lineitem_count; i++) {
        int32_t ok = db->l_orderkey[i];
        perm[write_pos[ok]++] = i;
    }

    // Apply permutation in parallel
    auto permute_i32 = [&](std::vector<int32_t>& v) {
        std::vector<int32_t> tmp(v.size());
        for (int64_t i = 0; i < (int64_t)v.size(); i++) tmp[i] = v[perm[i]];
        v = std::move(tmp);
    };
    auto permute_i64 = [&](std::vector<int64_t>& v) {
        std::vector<int64_t> tmp(v.size());
        for (int64_t i = 0; i < (int64_t)v.size(); i++) tmp[i] = v[perm[i]];
        v = std::move(tmp);
    };
    auto permute_char = [&](std::vector<char>& v) {
        std::vector<char> tmp(v.size());
        for (int64_t i = 0; i < (int64_t)v.size(); i++) tmp[i] = v[perm[i]];
        v = std::move(tmp);
    };
    auto permute_str = [&](std::vector<std::string>& v) {
        std::vector<std::string> tmp(v.size());
        for (int64_t i = 0; i < (int64_t)v.size(); i++) tmp[i] = std::move(v[perm[i]]);
        v = std::move(tmp);
    };

    // Parallel permutation application
    std::thread tp1([&]() {
        permute_i32(db->l_orderkey);
        permute_i32(db->l_partkey);
        permute_i32(db->l_suppkey);
        permute_i32(db->l_linenumber);
    });
    std::thread tp2([&]() {
        permute_i64(db->l_quantity);
        permute_i64(db->l_extendedprice);
        permute_i64(db->l_discount);
        permute_i64(db->l_tax);
    });
    std::thread tp3([&]() {
        permute_char(db->l_returnflag);
        permute_char(db->l_linestatus);
        permute_i32(db->l_shipdate);
        permute_i32(db->l_commitdate);
        permute_i32(db->l_receiptdate);
    });
    std::thread tp4([&]() {
        permute_str(db->l_shipinstruct);
        permute_str(db->l_shipmode);
    });

    tp1.join();
    tp2.join();
    tp3.join();
    tp4.join();

    return db;
}
