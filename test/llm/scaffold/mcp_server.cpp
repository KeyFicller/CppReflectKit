#include "bridge.h"

#include <iostream>

#include "app_server.h"

// MCP stdio entry: the concrete server over std::cin/std::cout, newline-delimited
// JSON-RPC 2.0. main() routes `--mcp` here ahead of the generic `--*` branch, so
// stdout stays a protocol stream rather than a single JSON document.
int bridge::run_mcp()
{
    app::CppReflectServer server;
    return server.serve(std::cin, std::cout);
}
