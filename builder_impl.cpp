#include "builder_impl.hpp"
#include <arrow/array.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <arrow/util/decimal.h>
#include <iostream>
#include <algorithm>

// Helper to get a column by name from an Arrow table
static std::shared_ptr<arrow::ChunkedArray> get_col(const std::shared_ptr<arrow::Table>& table, const std::string& name) {
    auto idx = table->schema()->GetFieldIndex(name);
    if (idx < 0) {
        std::cerr << "Column not found: " << name << "\n";
        return nullptr;
    }
    return table->column(idx);
}

// Extract string column
static void extract_strings(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<std::string>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            out.push_back(arr->GetString(i));
        }
    }
}

// Extract int32 column (handles both int32 and int64 source)
static void extract_int32(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int32_t>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto chunk = col->chunk(c);
        if (chunk->type_id() == arrow::Type::INT32) {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            for (int64_t i = 0; i < arr->length(); i++) {
                out.push_back(arr->Value(i));
            }
        } else if (chunk->type_id() == arrow::Type::INT64) {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            for (int64_t i = 0; i < arr->length(); i++) {
                out.push_back(static_cast<int32_t>(arr->Value(i)));
            }
        }
    }
}

// Extract int64 column (handles both int32 and int64 source)
static void extract_int64(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int64_t>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto chunk = col->chunk(c);
        if (chunk->type_id() == arrow::Type::INT64) {
            auto arr = std::static_pointer_cast<arrow::Int64Array>(chunk);
            for (int64_t i = 0; i < arr->length(); i++) {
                out.push_back(arr->Value(i));
            }
        } else if (chunk->type_id() == arrow::Type::INT32) {
            auto arr = std::static_pointer_cast<arrow::Int32Array>(chunk);
            for (int64_t i = 0; i < arr->length(); i++) {
                out.push_back(static_cast<int64_t>(arr->Value(i)));
            }
        }
    }
}

// Extract decimal128 column as int64 (raw unscaled value)
static void extract_decimal_as_cents(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int64_t>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Decimal128Array>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            // Decimal128 to int64 - assumes value fits in int64
            arrow::Decimal128 val(arr->GetValue(i));
            out.push_back(static_cast<int64_t>(val.IsNegative() ?
                -static_cast<int64_t>((-val).IsNegative() ? 0 : static_cast<uint64_t>((-val).low_bits())) :
                static_cast<int64_t>(val.low_bits())));
        }
    }
}

// Better decimal extraction: just get the low 64 bits treating as signed
static void extract_decimal(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<int64_t>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Decimal128Array>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            arrow::Decimal128 val(arr->GetValue(i));
            int64_t result;
            auto status = val.ToInteger(&result);
            (void)status; // assume fits
            out.push_back(result);
        }
    }
}

// Extract date32 column (days since epoch)
static void extract_date(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<Date>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::Date32Array>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            out.push_back(arr->Value(i));
        }
    }
}

// Extract single char column (from string column)
static void extract_char(const std::shared_ptr<arrow::ChunkedArray>& col, std::vector<char>& out) {
    out.reserve(col->length());
    for (int c = 0; c < col->num_chunks(); c++) {
        auto arr = std::static_pointer_cast<arrow::StringArray>(col->chunk(c));
        for (int64_t i = 0; i < arr->length(); i++) {
            auto sv = arr->GetView(i);
            out.push_back(sv.length() > 0 ? sv[0] : ' ');
        }
    }
}


Database* build(ParquetTables* pt) {
    auto db = new Database{};

    // ---- REGION ----
    {
        auto& t = pt->region;
        db->n_regions = t->num_rows();
        extract_strings(get_col(t, "r_name"), db->r_name);
        for (int i = 0; i < db->n_regions; i++) {
            db->region_name_to_key[db->r_name[i]] = i;
        }
    }

    // ---- NATION ----
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

    // ---- SUPPLIER ----
    {
        auto& t = pt->supplier;
        db->n_supplier = t->num_rows();
        extract_strings(get_col(t, "s_name"), db->s_name);
        extract_strings(get_col(t, "s_address"), db->s_address);
        extract_int32(get_col(t, "s_nationkey"), db->s_nationkey);
        extract_strings(get_col(t, "s_phone"), db->s_phone);
        extract_decimal(get_col(t, "s_acctbal"), db->s_acctbal);
        extract_strings(get_col(t, "s_comment"), db->s_comment);
    }

    // ---- CUSTOMER ----
    {
        auto& t = pt->customer;
        db->n_customer = t->num_rows();
        extract_strings(get_col(t, "c_name"), db->c_name);
        extract_strings(get_col(t, "c_address"), db->c_address);
        extract_int32(get_col(t, "c_nationkey"), db->c_nationkey);
        extract_strings(get_col(t, "c_phone"), db->c_phone);
        extract_decimal(get_col(t, "c_acctbal"), db->c_acctbal);
        extract_strings(get_col(t, "c_mktsegment"), db->c_mktsegment);
        extract_strings(get_col(t, "c_comment"), db->c_comment);
    }

    // ---- PART ----
    {
        auto& t = pt->part;
        db->n_part = t->num_rows();
        extract_strings(get_col(t, "p_name"), db->p_name);
        extract_strings(get_col(t, "p_mfgr"), db->p_mfgr);
        extract_strings(get_col(t, "p_brand"), db->p_brand);
        extract_strings(get_col(t, "p_type"), db->p_type);
        extract_int32(get_col(t, "p_size"), db->p_size);
        extract_strings(get_col(t, "p_container"), db->p_container);
        extract_decimal(get_col(t, "p_retailprice"), db->p_retailprice);
    }

    // ---- PARTSUPP ----
    {
        auto& t = pt->partsupp;
        db->n_partsupp = t->num_rows();
        extract_int32(get_col(t, "ps_partkey"), db->ps_partkey);
        extract_int32(get_col(t, "ps_suppkey"), db->ps_suppkey);
        extract_int32(get_col(t, "ps_availqty"), db->ps_availqty);
        extract_decimal(get_col(t, "ps_supplycost"), db->ps_supplycost);
    }

    // ---- ORDERS ----
    {
        auto& t = pt->orders;
        db->n_orders = t->num_rows();
        extract_int32(get_col(t, "o_orderkey"), db->o_orderkey);
        extract_int32(get_col(t, "o_custkey"), db->o_custkey);
        extract_char(get_col(t, "o_orderstatus"), db->o_orderstatus);
        extract_decimal(get_col(t, "o_totalprice"), db->o_totalprice);
        extract_date(get_col(t, "o_orderdate"), db->o_orderdate);
        extract_strings(get_col(t, "o_orderpriority"), db->o_orderpriority);
        extract_int32(get_col(t, "o_shippriority"), db->o_shippriority);
        extract_strings(get_col(t, "o_comment"), db->o_comment);

        // Build orderkey→index map
        db->max_orderkey = *std::max_element(db->o_orderkey.begin(), db->o_orderkey.end());
        db->orderkey_to_idx.assign(db->max_orderkey + 1, -1);
        for (int32_t i = 0; i < db->n_orders; i++) {
            db->orderkey_to_idx[db->o_orderkey[i]] = i;
        }
    }

    // ---- LINEITEM ----
    {
        auto& t = pt->lineitem;
        db->n_lineitem = t->num_rows();
        extract_int32(get_col(t, "l_orderkey"), db->l_orderkey);
        extract_int32(get_col(t, "l_partkey"), db->l_partkey);
        extract_int32(get_col(t, "l_suppkey"), db->l_suppkey);
        extract_int32(get_col(t, "l_linenumber"), db->l_linenumber);
        extract_decimal(get_col(t, "l_quantity"), db->l_quantity);
        extract_decimal(get_col(t, "l_extendedprice"), db->l_extendedprice);
        extract_decimal(get_col(t, "l_discount"), db->l_discount);
        extract_decimal(get_col(t, "l_tax"), db->l_tax);
        extract_char(get_col(t, "l_returnflag"), db->l_returnflag);
        extract_char(get_col(t, "l_linestatus"), db->l_linestatus);
        extract_date(get_col(t, "l_shipdate"), db->l_shipdate);
        extract_date(get_col(t, "l_commitdate"), db->l_commitdate);
        extract_date(get_col(t, "l_receiptdate"), db->l_receiptdate);
        extract_strings(get_col(t, "l_shipinstruct"), db->l_shipinstruct);
        extract_strings(get_col(t, "l_shipmode"), db->l_shipmode);

        // Build orderkey CSR index for lineitem (assumes lineitem sorted by orderkey from parquet)
        // Check if sorted
        db->lineitem_sorted_by_orderkey = true;
        for (int64_t i = 1; i < db->n_lineitem; i++) {
            if (db->l_orderkey[i] < db->l_orderkey[i-1]) {
                db->lineitem_sorted_by_orderkey = false;
                break;
            }
        }

        if (db->lineitem_sorted_by_orderkey) {
            db->orderkey_lineitem_start.assign(db->max_orderkey + 2, 0);
            db->orderkey_lineitem_end.assign(db->max_orderkey + 2, 0);
            int64_t i = 0;
            while (i < db->n_lineitem) {
                int32_t ok = db->l_orderkey[i];
                int64_t start = i;
                while (i < db->n_lineitem && db->l_orderkey[i] == ok) i++;
                db->orderkey_lineitem_start[ok] = start;
                db->orderkey_lineitem_end[ok] = i;
            }
        }
    }

    return db;
}
