#include "builder_api.hpp"
#include "loader_api.hpp"
#include "query_api.hpp"
#include "utils/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <stdexcept>

// Directory (relative to cwd) holding the plugin .so files. Overridden at
// compile time for variant builds (e.g. trace -> "./build_trace") so each
// binary loads its own instrumented libs instead of the default ones.
#ifndef PLUGIN_DIR
#define PLUGIN_DIR "./build"
#endif

struct State {
    std::string parquet_path;
    ParquetTables* parquet_tables = nullptr;
    Database* database = nullptr;
};

static State state;

static auto build_pipeline() {
    return make_pipeline(
        stage<RunPolicy::OnChange>(PLUGIN_DIR "/libloader.so", [](Plugin& plugin) {
            auto api = plugin.get<LoaderApi>();
            std::cerr << "loader start\n";
            const auto t0 = std::chrono::steady_clock::now();
            state.parquet_tables = api.load(state.parquet_path);
            const auto t1 = std::chrono::steady_clock::now();
            std::cerr << "loader done\n";
            // Framework-level load timing: emitted by the engine itself so it
            // is reliable regardless of what the loader plugin prints.
            const float ms =
                std::chrono::duration<float, std::milli>(t1 - t0).count();
            std::cerr << "Load ms: " << ms << "\n";
            return 0;
        }, [](Plugin& plugin) {
            // Free the previous dataset (built by the outgoing libloader.so)
            // before the hot-reload, otherwise it leaks inside this long-lived
            // loader process on every reload.
            if (state.parquet_tables) {
                plugin.get<LoaderApi>().destroy(state.parquet_tables);
                state.parquet_tables = nullptr;
            }
        }),
        stage<RunPolicy::OnChange>(PLUGIN_DIR "/libbuilder.so", [](Plugin& plugin, int) {
            auto api = plugin.get<BuilderApi>();
            std::cerr << "builder start\n";
            const auto t0 = std::chrono::steady_clock::now();
            state.database = api.build(state.parquet_tables);
            std::cerr << "builder done\n";
            const auto t1 = std::chrono::steady_clock::now();
            const float ms =
                std::chrono::duration<float, std::milli>(t1 - t0).count();
            std::cerr << "Ingest ms: " << ms << "\n";
            return 0;
        }, [](Plugin& plugin) {
            // Free the previous in-memory database (built by the outgoing
            // libbuilder.so) before the hot-reload, otherwise it leaks inside
            // this long-lived builder process on every reload.
            if (state.database) {
                plugin.get<BuilderApi>().destroy(state.database);
                state.database = nullptr;
            }
        }),
        stage<RunPolicy::Always>(PLUGIN_DIR "/libquery.so", [](Plugin& plugin, int) {
            auto api = plugin.get<QueryApi>();
            std::cerr << "query start\n";
            const auto t0 = std::chrono::steady_clock::now();
            api.query(state.database);
            const auto t1 = std::chrono::steady_clock::now();
            std::cerr << "query done\n";
            // Framework-level query timing: emitted by the engine itself so it
            // does not depend on LLM-generated print statements inside the
            // query plugin (which are unreliable / often absent). A single
            // query is executed per run during optimization measurement, so
            // this is that query's execution time. Microsecond resolution is
            // surfaced for sub-millisecond queries.
            const long us = static_cast<long>(
                std::chrono::duration<double, std::micro>(t1 - t0).count());
            std::cerr << "Execution ms: " << (us / 1000) << " (" << us
                      << " us)\n";
            return 0;
        }));
}

static void run_child(int read_fd, int done_fd) {
    auto pipeline = build_pipeline();
    pipeline.run(read_fd, done_fd, false);
}

static int getenv_fd(const char* name) {
    const char* v = std::getenv(name);
    if (!v) {
        throw std::runtime_error(std::string(name) + " not supplied");
    }
    return std::atoi(v);
}


static void run_parent(PipelineControl& control) {
    int in_fd = getenv_fd("P2C_FD");  // read from parent
    int out_fd = getenv_fd("C2P_FD"); // write to parent

    std::ifstream in("/proc/self/fd/" + std::to_string(in_fd));
    if (!in.is_open()) {
        throw std::runtime_error("open P2C_FD failed");
    }
    std::ofstream out("/proc/self/fd/" + std::to_string(out_fd));
    if (!out.is_open()) {
        throw std::runtime_error("open C2P_FD failed");
    }

    std::string cmd;
    while (std::getline(in, cmd)) {
        std::cerr << "got: " << cmd << "\n";

        if (cmd == "stop") {
            break;
        }
        if (cmd != "run") {
            throw std::runtime_error("invalid command");
        }

        control.send_run();
        DoneToken token = control.read_done();
        std::cerr << "exit_code: " << token.exit_code << " signal: " << token.term_signal
                  << "\n";
        out << "exit_code: " << token.exit_code << " signal: " << token.term_signal
            << "\n";
        out.flush();
    }

    control.send_terminate();
}


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <PARQUET_DIR\n";
        return 1;
    }
    std::string base_parquet = argv[1];
    state.parquet_path = base_parquet;

    signal(SIGPIPE, SIG_IGN);
    int p2c[2];
    int done_pipe[2];
    if (pipe(p2c) == -1) {
        perror("pipe");
        return 1;
    }
    if (pipe(done_pipe) == -1) {
        perror("pipe");
        close(p2c[0]);
        close(p2c[1]);
        return 1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        install_parent_death_signal();
        close(p2c[1]);
        close(done_pipe[0]);
        run_child(p2c[0], done_pipe[1]);
        _exit(0);
    }
    if (pid < 0) {
        perror("fork");
        close(p2c[0]);
        close(p2c[1]);
        close(done_pipe[0]);
        close(done_pipe[1]);
        return 1;
    }

    close(p2c[0]);
    close(done_pipe[1]);
    PipelineControl control(p2c[1], done_pipe[0], true);
    run_parent(control);
    waitpid(pid, nullptr, 0);
    return 0;
}
