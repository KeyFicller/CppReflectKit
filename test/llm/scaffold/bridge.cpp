#include "bridge.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app_server.h"
#include "llm/reflection_json.h"

// --serve() is a Unix socket server, so this translation unit is POSIX-only.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// The example domain lives in app_model.h so the MCP server and this CLI share
// one set of declarations; only the model registration stays here, in the
// namespace the macro expects, so exactly one TU runs the static initializers.
namespace app {
CPP_REFLECT_MODEL(Range)
CPP_REFLECT_MODEL(Query)
CPP_REFLECT_MODEL(SearchRequest)
} // namespace app

namespace {

using json = nlohmann::json;

std::optional<json> parse_json(std::string_view _text) {
    try {
        return json::parse(_text);
    } catch (const json::exception &) {
        return std::nullopt;
    }
}

/// The tool table projected as OpenAI tools. The same instance that serves MCP
/// supplies this, so --emit-tools and MCP tools/list can never disagree.
std::string tool_schemas_json() {
    app::CppReflectServer server;
    return server.tools_as_openai_json();
}

/// Every registered model's JSON Schema, keyed by name. The schema is what the
/// model's structured output must conform to.
std::string response_schemas_json() {
    std::string out = "{";
    std::size_t n = 0;
    for (const auto &entry : js::model_registry()) {
        if (n++) {
            out += ",";
        }
        out +=
            "\"" + std::string(entry.name) + "\":" + std::string(entry.schema);
    }
    out += "}";
    return out;
}

/// The whole of `_in` parsed as JSON, where `_what` names the payload for the
/// error message.
json parse_stdin(std::istream &_in, const char *_what) {
    const std::string input{std::istreambuf_iterator<char>(_in),
                            std::istreambuf_iterator<char>()};
    const auto doc = parse_json(input);
    if (!doc) {
        throw std::runtime_error(std::string("invalid JSON ") + _what +
                                 " on stdin");
    }
    return *doc;
}

std::string call_tool_json(std::string_view _name, std::istream &_in) {
    const json args = parse_stdin(_in, "arguments");
    app::CppReflectServer server;
    return server.call_tool(_name, args);
}

std::string parse_response_json(std::string_view _name, std::istream &_in) {
    const json doc = parse_stdin(_in, "response");
    for (const auto &entry : js::model_registry()) {
        if (entry.name != _name) {
            continue;
        }
        try {
            // dump() re-serializes the reconstructed struct from reflection, so
            // no per-struct printing lives here.
            return entry.dump(doc);
        } catch (const std::exception &e) {
            throw std::runtime_error("parse into " + std::string(_name) +
                                     " failed: " + e.what());
        }
    }
    throw std::runtime_error("unknown model: " + std::string(_name));
}

} // namespace

std::string bridge::run_request(const std::vector<std::string> &_args,
                                std::istream &_in) {
    if (_args.empty()) {
        throw std::runtime_error("no command");
    }
    const std::string &command = _args[0];
    if (command == "--emit-tools") {
        return tool_schemas_json();
    }
    if (command == "--emit-schemas") {
        return response_schemas_json();
    }
    if (command == "--call") {
        return call_tool_json(_args.size() > 1 ? _args[1] : "", _in);
    }
    if (command == "--parse") {
        return parse_response_json(_args.size() > 1 ? _args[1] : "", _in);
    }
    throw std::runtime_error("unknown command: " + command);
}

// --- --serve: the joint-debug transport --------------------------------------
// A debugger must own the process to set breakpoints, but Python must own the
// request channel. Both cannot be stdin/stdout, so the channel is out of band:
// lldb launches the process once and holds it open, and the Python side
// connects to the socket instead of spawning. Every request of a whole LLM loop
// then hits the same breakpoints in the same session.
//
// One JSON object per line each way:
//   request   {"args":["--call","search_documents"], "stdin":{...}}
//   response  {"stdout":"..."} or {"error":"..."}

namespace {

void write_all(int _fd, std::string_view _text) {
    for (std::size_t sent = 0; sent < _text.size();) {
        const ssize_t written =
            ::write(_fd, _text.data() + sent, _text.size() - sent);
        if (written <= 0) {
            return;
        }
        sent += static_cast<std::size_t>(written);
    }
}

/// Runs one request line and writes exactly one response line.
void respond(int _fd, const std::string &_line) {
    json reply;
    try {
        const json request = json::parse(_line);
        const std::vector<std::string> args =
            request.value("args", std::vector<std::string>{});
        const bool has_stdin =
            request.contains("stdin") && !request.at("stdin").is_null();
        std::istringstream in(has_stdin ? request.at("stdin").dump()
                                        : std::string{});
        reply["stdout"] = bridge::run_request(args, in);
    } catch (const std::exception &e) {
        reply["error"] = e.what();
    }
    write_all(_fd, reply.dump() + "\n");
}

/// Reads newline-delimited requests from one client until it disconnects.
void serve_client(int _fd) {
    std::string pending;
    char chunk[4096];
    for (;;) {
        const ssize_t received = ::read(_fd, chunk, sizeof chunk);
        if (received <= 0) {
            return;
        }
        pending.append(chunk, static_cast<std::size_t>(received));
        std::size_t newline;
        while ((newline = pending.find('\n')) != std::string::npos) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty()) {
                respond(_fd, line);
            }
        }
    }
}

} // namespace

int bridge::serve(std::string_view _socket_path) {
    // A client that disconnects mid-session must not take the debuggee down.
    ::signal(SIGPIPE, SIG_IGN);

    const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        std::cerr << "serve: socket: " << std::strerror(errno) << '\n';
        return 1;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (_socket_path.size() >= sizeof(address.sun_path)) {
        std::cerr << "serve: socket path too long\n";
        return 1;
    }
    std::memcpy(address.sun_path, _socket_path.data(), _socket_path.size());

    const std::string path(_socket_path);
    ::unlink(path.c_str());
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) < 0 ||
        ::listen(listener, 1) < 0) {
        std::cerr << "serve: bind/listen " << path << ": "
                  << std::strerror(errno) << '\n';
        return 1;
    }

    std::cerr << "serve: listening on " << path << '\n';
    // Never returns: the caller is a debugger holding the process open.
    for (;;) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            continue;
        }
        serve_client(client);
        ::close(client);
    }
}
