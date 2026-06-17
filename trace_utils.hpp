#pragma once

// Tracing / profiling instrumentation.
//
// All instrumentation is behind the TRACE macro. When TRACE is NOT defined,
// every macro below expands to nothing (or a no-op cast), guaranteeing zero
// runtime overhead and no extra allocations or I/O.
//
// Output format (emitted once per query, to stderr so the CSV result on
// stdout stays clean and parsable):
//
//     PROFILE <section_name> <nanoseconds>
//     COUNT   <counter_name> <value>
//
// Section / counter names should encode the query and phase, e.g.
//   q1_scan_total, q1_agg, q3_join_probe, q3_sort, q1_rows_scanned, ...

#ifdef TRACE

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <vector>

inline uint64_t trace_get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

struct TraceRecord {
    const char* name;
    uint64_t value;
};

// Accumulates records during a single query and flushes them all at once at
// query end, so there is no I/O inside hot loops.
struct TraceState {
    std::vector<TraceRecord> timings;
    std::vector<TraceRecord> counts;

    void add_timing(const char* name, uint64_t ns) {
        timings.push_back({name, ns});
    }
    void add_count(const char* name, uint64_t value) {
        counts.push_back({name, value});
    }
    void flush() {
        for (const auto& r : timings) {
            std::fprintf(stderr, "PROFILE %s %llu\n", r.name,
                         static_cast<unsigned long long>(r.value));
        }
        for (const auto& r : counts) {
            std::fprintf(stderr, "COUNT %s %llu\n", r.name,
                         static_cast<unsigned long long>(r.value));
        }
        std::fflush(stderr);
        timings.clear();
        counts.clear();
    }
};

inline TraceState& trace_state() {
    static TraceState s;
    return s;
}

// RAII scoped timer: records elapsed nanoseconds for the named section when it
// goes out of scope. Place around whole operator phases (never inside per-row
// hot loops) so exactly one timing record is produced per phase.
class ScopedTimer {
    const char* name_;
    uint64_t start_ns_;
public:
    explicit ScopedTimer(const char* name)
        : name_(name), start_ns_(trace_get_time_ns()) {}
    ~ScopedTimer() {
        trace_state().add_timing(name_, trace_get_time_ns() - start_ns_);
    }
};

#define TRACE_CONCAT_(a, b) a##b
#define TRACE_CONCAT(a, b) TRACE_CONCAT_(a, b)

// Scoped wall-clock timer for an operator phase.
#define PROFILE_SCOPE(name) ScopedTimer TRACE_CONCAT(_trace_timer_, __LINE__)(name)

// Declare / mutate a row counter. Disappear completely without TRACE.
#define TRACE_DECL(var) uint64_t var = 0
#define TRACE_INC(var) (++(var))
#define TRACE_ADD(var, n) ((var) += static_cast<uint64_t>(n))

// Emit a COUNT record.
#define TRACE_COUNT(name, value) \
    trace_state().add_count(name, static_cast<uint64_t>(value))

// Flush all accumulated records for the current query (call once at query end).
#define TRACE_FLUSH() trace_state().flush()

#else  // !TRACE

#define PROFILE_SCOPE(name) ((void)0)
#define TRACE_DECL(var)
#define TRACE_INC(var) ((void)0)
#define TRACE_ADD(var, n) ((void)0)
#define TRACE_COUNT(name, value) ((void)0)
#define TRACE_FLUSH() ((void)0)

#endif  // TRACE
