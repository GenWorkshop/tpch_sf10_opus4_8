#include "builder_impl.hpp"
#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/util/decimal.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <vector>
#include <functional>
#include <cstring>

// Helper to get a column by name from an Arrow table
static std::shared_ptr<arrow::ChunkedArray> get_col(const std::shared_ptr<arrow::Table>& table, const std::string& name) {
    auto idx = table->schema()->GetFieldIndex(name);
    if (idx < 0) {
        std::cerr << "Column not found: " << name << "\n";
        return nullptr;
    }
    return table->column(idx);
}

// Extract string column (pre-sized)
static void extract_strings(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<std::string>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            out[offset++] = arr->GetString(i);
        }
    }
}

// Extract int32 column (handles both int32 and int64 source, uses memcpy where possible)
static void extract_int32(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int32_t>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto chunk = col->chunk(c);
        if (chunk->type_id() == arrow::Type::INT32) {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            const int32_t* raw = arr->raw_values();
            std::memcpy(out.data() + offset, raw, arr->length() * sizeof(int32_t));
            offset += arr->length();
        } else if (chunk->type_id() == arrow::Type::INT64) {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            const int64_t* raw = arr->raw_values();
            for (int64_t i = 0; i < arr->length(); i++) {
                out[offset++] = static_cast<int32_t>(raw[i]);
            }
        }
    }
}

// Extract int64 column (handles both int32 and int64 source, uses memcpy where possible)
static void extract_int64(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int64_t>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto chunk = col->chunk(c);
        if (chunk->type_id() == arrow::Type::INT64) {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            const int64_t* raw = arr->raw_values();
            std::memcpy(out.data() + offset, raw, arr->length() * sizeof(int64_t));
            offset += arr->length();
        } else if (chunk->type_id() == arrow::Type::INT32) {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            const int32_t* raw = arr->raw_values();
            for (int64_t i = 0; i < arr->length(); i++) {
                out[offset++] = static_cast<int64_t>(raw[i]);
            }
        }
    }
}

// Extract decimal128 column as int64
static void extract_decimal(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int64_t>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Decimal128Array>(col->chunk(c));
        // Decimal128 is stored as 16 bytes (little-endian), low 8 bytes are the int64 value
        // for values that fit in int64 (which all TPC-H values do)
        const uint8_t* raw_data = arr->values()->data() + arr->offset() * 16;
        for (int64_t i = 0; i < arr->length(); i++) {
            int64_t result;
            std::memcpy(&result, raw_data + i * 16, sizeof(int64_t));
            out[offset++] = result;
        }
    }
}

// Extract date32 column (days since epoch) - direct memcpy
static void extract_date(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<Date>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Date32Array>(col->chunk(c));
        const int32_t* raw = arr->raw_values();
        std::memcpy(out.data() + offset, raw, arr->length() * sizeof(int32_t));
        offset += arr->length();
    }
}

// Extract single char column (from string column)
static void extract_char(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<char>& out) {
    out.resize(col->length());
    int64_t offset = 0;
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            auto sv = arr->GetView(i);
            out[offset++] = sv.length() > 0 ? sv[0] : ' ';
        }
    }
}

// Run multiple tasks in parallel
static void parallel_run(std::vector<std::function<void()>>& tasks) {
    std::vector<std::thread> threads;
    threads.reserve(tasks.size());
    for (auto& task : tasks) {
        threads.emplace_back(task);
    }
    for (auto& t : threads) {
        t.join();
    }
}


Database* build(ParquetTables* pt) {
    auto db = new Database{};

    // ---- REGION & NATION (tiny, do first sequentially) ----
    {
        auto& t = pt->region;
        db->n_regions = t->num_rows();
        extract_strings(get_col(t, "r_name"), db->r_name);
        for (int i = 0; i < db->n_regions; i++) {
            db->region_name_to_key[db->r_name[i]] = i;
        }
    }
    {
        auto& t = pt->nation;
        db->n_nations = t->num_rows();
        extract_strings(get_col(t, "n_name"), db->n_name);
        extract_int32(get_col(t, "n_regionkey"), db->n_regionkey);
        for (int i = 0; i < db->n_nations; i++) {
            db->nation_name_to_key[db->n_name[i]] = i;
        }
        db->nations_in_region.resize(db->n_regions);
        for (int i = 0; i < db->n_nations; i++) {
            db->nations_in_region[db->n_regionkey[i]].push_back(i);
        }
    }

    // Set row counts for all tables
    db->n_supplier = pt->supplier->num_rows();
    db->n_customer = pt->customer->num_rows();
    db->n_part = pt->part->num_rows();
    db->n_partsupp = pt->partsupp->num_rows();
    db->n_orders = pt->orders->num_rows();
    db->n_lineitem = pt->lineitem->num_rows();

    // ---- Extract ALL columns in parallel across all tables ----
    {
        std::vector<std::function<void()>> tasks;

        // Supplier
        tasks.push_back([&]{ extract_strings(get_col(pt->supplier, "s_name"), db->s_name); });
        tasks.push_back([&]{ extract_strings(get_col(pt->supplier, "s_address"), db->s_address); });
        tasks.push_back([&]{ extract_int32(get_col(pt->supplier, "s_nationkey"), db->s_nationkey); });
        tasks.push_back([&]{ extract_strings(get_col(pt->supplier, "s_phone"), db->s_phone); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->supplier, "s_acctbal"), db->s_acctbal); });
        tasks.push_back([&]{ extract_strings(get_col(pt->supplier, "s_comment"), db->s_comment); });

        // Customer
        tasks.push_back([&]{ extract_strings(get_col(pt->customer, "c_name"), db->c_name); });
        tasks.push_back([&]{ extract_strings(get_col(pt->customer, "c_address"), db->c_address); });
        tasks.push_back([&]{ extract_int32(get_col(pt->customer, "c_nationkey"), db->c_nationkey); });
        tasks.push_back([&]{ extract_strings(get_col(pt->customer, "c_phone"), db->c_phone); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->customer, "c_acctbal"), db->c_acctbal); });
        tasks.push_back([&]{ extract_strings(get_col(pt->customer, "c_mktsegment"), db->c_mktsegment); });
        tasks.push_back([&]{ extract_strings(get_col(pt->customer, "c_comment"), db->c_comment); });

        // Part
        tasks.push_back([&]{ extract_strings(get_col(pt->part, "p_name"), db->p_name); });
        tasks.push_back([&]{ extract_strings(get_col(pt->part, "p_mfgr"), db->p_mfgr); });
        tasks.push_back([&]{ extract_strings(get_col(pt->part, "p_brand"), db->p_brand); });
        tasks.push_back([&]{ extract_strings(get_col(pt->part, "p_type"), db->p_type); });
        tasks.push_back([&]{ extract_int32(get_col(pt->part, "p_size"), db->p_size); });
        tasks.push_back([&]{ extract_strings(get_col(pt->part, "p_container"), db->p_container); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->part, "p_retailprice"), db->p_retailprice); });

        // Partsupp
        tasks.push_back([&]{ extract_int32(get_col(pt->partsupp, "ps_partkey"), db->ps_partkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->partsupp, "ps_suppkey"), db->ps_suppkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->partsupp, "ps_availqty"), db->ps_availqty); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->partsupp, "ps_supplycost"), db->ps_supplycost); });

        // Orders
        tasks.push_back([&]{ extract_int32(get_col(pt->orders, "o_orderkey"), db->o_orderkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->orders, "o_custkey"), db->o_custkey); });
        tasks.push_back([&]{ extract_char(get_col(pt->orders, "o_orderstatus"), db->o_orderstatus); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->orders, "o_totalprice"), db->o_totalprice); });
        tasks.push_back([&]{ extract_date(get_col(pt->orders, "o_orderdate"), db->o_orderdate); });
        tasks.push_back([&]{ extract_strings(get_col(pt->orders, "o_orderpriority"), db->o_orderpriority); });
        tasks.push_back([&]{ extract_int32(get_col(pt->orders, "o_shippriority"), db->o_shippriority); });
        tasks.push_back([&]{ extract_strings(get_col(pt->orders, "o_comment"), db->o_comment); });

        // Lineitem (largest table)
        tasks.push_back([&]{ extract_int32(get_col(pt->lineitem, "l_orderkey"), db->l_orderkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->lineitem, "l_partkey"), db->l_partkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->lineitem, "l_suppkey"), db->l_suppkey); });
        tasks.push_back([&]{ extract_int32(get_col(pt->lineitem, "l_linenumber"), db->l_linenumber); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->lineitem, "l_quantity"), db->l_quantity); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->lineitem, "l_extendedprice"), db->l_extendedprice); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->lineitem, "l_discount"), db->l_discount); });
        tasks.push_back([&]{ extract_decimal(get_col(pt->lineitem, "l_tax"), db->l_tax); });
        tasks.push_back([&]{ extract_char(get_col(pt->lineitem, "l_returnflag"), db->l_returnflag); });
        tasks.push_back([&]{ extract_char(get_col(pt->lineitem, "l_linestatus"), db->l_linestatus); });
        tasks.push_back([&]{ extract_date(get_col(pt->lineitem, "l_shipdate"), db->l_shipdate); });
        tasks.push_back([&]{ extract_date(get_col(pt->lineitem, "l_commitdate"), db->l_commitdate); });
        tasks.push_back([&]{ extract_date(get_col(pt->lineitem, "l_receiptdate"), db->l_receiptdate); });
        tasks.push_back([&]{ extract_strings(get_col(pt->lineitem, "l_shipinstruct"), db->l_shipinstruct); });
        tasks.push_back([&]{ extract_strings(get_col(pt->lineitem, "l_shipmode"), db->l_shipmode); });

        parallel_run(tasks);
    }

    // ---- Post-processing (depends on extracted data) ----

    // Build orderkey->index map
    db->max_orderkey = *std::max_element(db->o_orderkey.begin(), db->o_orderkey.end());
    db->orderkey_to_idx.assign(db->max_orderkey + 1, -1);
    bool ord_sorted = true;
    for (int32_t i = 0; i < db->n_orders; i++) {
        db->orderkey_to_idx[db->o_orderkey[i]] = i;
        if (i > 0 && db->o_orderkey[i] < db->o_orderkey[i-1]) ord_sorted = false;
    }
    db->orders_sorted_by_orderkey = ord_sorted;

    // Build orderkey CSR index for lineitem
    db->lineitem_sorted_by_orderkey = true;
    for (int64_t i = 1; i < db->n_lineitem; i++) {
        if (db->l_orderkey[i] < db->l_orderkey[i-1]) {
            db->lineitem_sorted_by_orderkey = false;
            break;
        }
    }

    if (db->lineitem_sorted_by_orderkey) {
        db->orderkey_lineitem_range.assign(db->max_orderkey + 2, {0, 0});
        int64_t i = 0;
        while (i < db->n_lineitem) {
            int32_t ok = db->l_orderkey[i];
            int64_t start = i;
            while (i < db->n_lineitem && db->l_orderkey[i] == ok) i++;
            db->orderkey_lineitem_range[ok] = {(int32_t)start, (int32_t)i};
        }
    }

    return db;
}
