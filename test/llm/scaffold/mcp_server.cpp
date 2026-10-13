#include "bridge.h"

#include <iostream>

#include "app_server.h"

// MCP entries. `--mcp` is the local transport (an agent owns the process).
// `--http` is the external one (this process is already up; agents POST /mcp).
int bridge::run_mcp() {
    app::CppReflectServer server;
    return server.stdio();
}

int bridge::run_http(int _port) {
    app::CppReflectServer server;
    return server.serve_http(_port);
}
