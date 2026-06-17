#pragma once

#include <arrow/table.h>
#include <memory>

struct ParquetTables {
    using ArrowTable = std::shared_ptr<arrow::Table>;

    // start: table-defs
    ArrowTable region;
    ArrowTable nation;
    ArrowTable supplier;
    ArrowTable customer;
    ArrowTable part;
    ArrowTable partsupp;
    ArrowTable orders;
    ArrowTable lineitem;
    // end: table-defs
};


ParquetTables* load(std::string);
