#include "bridge.h"

#include <cstring>
#include <exception>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#include <gtest/gtest.h>

// The app entry point: the process llm_client.py spawns. The CLI/MCP modes are
// routed first; with no arguments (or gtest's own flags) it runs the runtime
// tests defined in this directory.
int main(int argc, char* argv[]) {

    // Long-running mode for joint debugging: lldb launches this once and holds
    // it open while Python drives through the socket.
    if (argc > 1 && std::strcmp(argv[1], "--serve") == 0) {
        return bridge::serve(argc > 2 ? argv[2] : "/tmp/cpp_reflect.sock");
    }

    // MCP server mode: newline-delimited JSON-RPC 2.0 on stdin/stdout. Kept
    // ahead of the "--" CLI branch below, whose stdout must stay a single JSON
    // document rather than a protocol stream.
    if (argc > 1 && std::strcmp(argv[1], "--mcp") == 0) {
        return bridge::run_mcp();
    }

    // gtest's own flags (--gtest_list_tests, --gtest_filter, ...) must reach gtest,
    // not the CLI branch below, which rejects anything starting with "--".
    const bool gtest_flag = argc > 1 && std::strncmp(argv[1], "--gtest", 7) == 0;

    // CLI modes used by the Python LLM loop.
    if (!gtest_flag && argc > 1 && std::strncmp(argv[1], "--", 2) == 0) {
        const std::vector<std::string> args(argv + 1, argv + argc);
        try {
            std::println("{}", bridge::run_request(args, std::cin));
            return 0;
        } catch (const std::exception& e) {
            std::cerr << e.what() << '\n';
            return 1;
        }
    }

    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
