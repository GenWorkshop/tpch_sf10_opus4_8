#include "builder_api.hpp"

#include "builder_impl.hpp"
#include "utils/plugin_base.hpp"

// Free a Database produced by build(). Defined here, where the complete type
// from builder_impl.hpp is visible, so the engine can release the previous
// in-memory database before a hot-reload instead of leaking it inside the
// long-lived builder process.
static void destroy_database(Database* database) {
    delete database;
}

static const BuilderApi BUILDER = {
    .build = &build,
    .destroy = &destroy_database,
};

extern "C" __attribute__((visibility("default")))
const void*
plugin_query() {
    return &BUILDER;
}
