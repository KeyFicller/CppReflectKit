#pragma once

#include <istream>
#include <string>
#include <string_view>
#include <vector>

// The runtime entry points the C++ side exposes outward: the `--*` CLI that
// llm_client.py drives, the `--serve` socket used for joint debugging, and the
// MCP stdio loop. main() is the only caller. Everything here is transport
// around the one dispatch in run_request, so nothing in this header is test
// code.
namespace bridge {

/// Runs one CLI-shaped request without touching std::cin/std::cout: `_args` is
/// argv[1..], and `_in` supplies stdin for the commands that take it (it is
/// left untouched otherwise). Returns the text that belongs on stdout, or
/// throws std::runtime_error. main() and serve() share this, so a session under
/// a debugger dispatches exactly like a plain CLI invocation.
std::string run_request(const std::vector<std::string> &_args,
                        std::istream &_in);

/// Serves run_request over newline-delimited JSON on a Unix socket, so a
/// debugger can hold one process open across many requests. Does not return.
int serve(std::string_view _socket_path);

/// Local MCP: stdin/stdout. Returns when stdin reaches EOF.
int run_mcp();

/// External MCP: Streamable HTTP on 127.0.0.1:`_port`, path `/mcp`. Blocks
/// until the process is signaled to exit. Returns 1 if the port will not bind.
int run_http(int _port);

} // namespace bridge
