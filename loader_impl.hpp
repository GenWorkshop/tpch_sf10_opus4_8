#pragma once

#include <arrow/table.h>
#include <memory>

struct ParquetTables {
    using ArrowTable = std::shared_ptr<arrow::Table>;

    // start: table-defs
    // end: table-defs
};


ParquetTables* load(std::string);
