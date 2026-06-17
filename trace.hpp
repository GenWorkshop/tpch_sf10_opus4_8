#pragma once
// trace.hpp — lightweight, zero-overhead-when-disabled tracing instrumentation.
//
// Build with -DTRACE to enable.  When TRACE is not defined every macro below
// expands to a no-op, so there is no runtime cost and no symbols are emitted.
//
// Output is written to stderr once per query (via trace_flush()) using a
// stable, machine-parsable format with exactly two line types:
//
//     PROFILE <section_name> <nanoseconds>
//     COUNT   <counter_name> <value>
//
// Section / counter names should embed the query id and phase, e.g.
//     q1_scan_total, q1_agg, q1_rows_scanned, ...
//
// Timing instrumentation uses RAII scoped timers (PROFILE_SCOPE) and must only
// wrap whole phases (scan / join / aggregation / sort), never per-row hot
// loops.  Row counters are plain integer locals (TRACE_DECL_COUNTER / TRACE_INC
// / TRACE_ADD) that are emitted once at the end via TRACE_COUNT — so the hot
// path performs only register increments with no allocation or I/O.

#ifdef TRACE

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>
#include <utility>

namespace trace {

inline uint64_t get_time_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Accumulated records for the query currently executing.  These are appended
// to outside hot loops (scope exit for timings, once-at-end for counters), so
// the small dynamic allocations here never touch a per-row path.
inline std::vector<std::pair<const char*, uint64_t>>& timings() {
    static std::vector<std::pair<const char*, uint64_t>> v;
    return v;
}
inline std::vector<std::pair<const char*, uint64_t>>& counters() {
    static std::vector<std::pair<const char*, uint64_t>> v;
    return v;
}

inline void record_timing(const char* name, uint64_t ns) {
    timings().emplace_back(name, ns);
}
inline void record_count(const char* name, uint64_t value) {
    counters().emplace_back(name, value);
}

class ScopedTimer {
    const char* name_;
    uint64_t start_ns_;
public:
    explicit ScopedTimer(const char* n) : name_(n), start_ns_(get_time_ns()) {}
    ~ScopedTimer() { record_timing(name_, get_time_ns() - start_ns_); }
};

// Emit all accumulated records for the current query to stderr, then clear so
// the next query starts fresh.  Called once per query at the end.
inline void flush() {
    for (auto& t : timings()) {
        std::fprintf(stderr, "PROFILE %s %llu\n", t.first,
                     static_cast<unsigned long long>(t.second));
    }
    for (auto& c : counters()) {
        std::fprintf(stderr, "COUNT %s %llu\n", c.first,
                     static_cast<unsigned long long>(c.second));
    }
    std::fflush(stderr);
    timings().clear();
    counters().clear();
}

} // namespace trace

#define TRACE_CAT2(a, b) a##b
#define TRACE_CAT(a, b) TRACE_CAT2(a, b)
#define PROFILE_SCOPE(name) trace::ScopedTimer TRACE_CAT(_trace_timer_, __LINE__)(name)
#define TRACE_DECL_COUNTER(var) uint64_t var = 0
#define TRACE_INC(var) (++(var))
#define TRACE_ADD(var, n) ((var) += static_cast<uint64_t>(n))
#define TRACE_COUNT(name, var) trace::record_count(name, (var))
#define TRACE_FLUSH() trace::flush()

#else // !TRACE

#define PROFILE_SCOPE(name) ((void)0)
#define TRACE_DECL_COUNTER(var) ((void)0)
#define TRACE_INC(var) ((void)0)
#define TRACE_ADD(var, n) ((void)0)
#define TRACE_COUNT(name, var) ((void)0)
#define TRACE_FLUSH() ((void)0)

#endif // TRACE
