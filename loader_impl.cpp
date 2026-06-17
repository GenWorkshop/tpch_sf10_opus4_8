#include "loader_impl.hpp"

#include "loader_utils.hpp"


ParquetTables* load(std::string path) {
    auto tables = new ParquetTables{};

    // start: table-reads
    tables->region   = ReadParquetTable(path + "/region.parquet");
    tables->nation   = ReadParquetTable(path + "/nation.parquet");
    tables->part     = ReadParquetTable(path + "/part.parquet");
    tables->supplier = ReadParquetTable(path + "/supplier.parquet");
    tables->partsupp = ReadParquetTable(path + "/partsupp.parquet");
    tables->customer = ReadParquetTable(path + "/customer.parquet");
    tables->orders   = ReadParquetTable(path + "/orders.parquet");
    tables->lineitem = ReadParquetTable(path + "/lineitem.parquet");
    // end: table-reads

    return tables;
}
