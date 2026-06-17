#include "loader_api.hpp"

#include "loader_impl.hpp"
#include "utils/plugin_base.hpp"


// Free a ParquetTables produced by load(). Defined here, where the complete
// type from loader_impl.hpp is visible, so the engine can release the previous
// dataset before a hot-reload instead of leaking it inside the long-lived
// loader process.
static void destroy_tables(ParquetTables* tables) {
    delete tables;
}

static const LoaderApi LOADER = {
    .load = &load,
    .destroy = &destroy_tables,
};

extern "C" __attribute__((visibility("default")))
const void*
plugin_query() {
    return &LOADER;
}
