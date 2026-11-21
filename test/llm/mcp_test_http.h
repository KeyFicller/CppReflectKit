#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "llm/reflection_mcp.h"

namespace mcp_test {

using json = nlohmann::json;

inline json request(std::string_view _method, json _params = json::object()) {
    json r{{"jsonrpc", "2.0"}, {"id", 1}, {"method", _method}};
    if (!_params.empty()) {
        r["params"] = std::move(_params);
    }
    return r;
}

inline json meta_version(std::string_view _version = mcp::kProtocolVersion) {
    return json{
        {"_meta",
         {{"io.modelcontextprotocol/protocolVersion", std::string(_version)}}}};
}

template <class S> struct HttpServerOf {
    S server;
    std::thread thread;

    template <class... Args>
    explicit HttpServerOf(Args &&..._args)
        : server(std::forward<Args>(_args)...),
          thread([this] { server.serve_http(0); }) {
        for (int i = 0; i < 100 && server.http_port() < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~HttpServerOf() {
        server.stop_http();
        if (thread.joinable()) {
            thread.join();
        }
    }

    httplib::Result post(const json &_body, httplib::Headers _headers) {
        _headers.emplace("Accept", "application/json, text/event-stream");
        _headers.emplace("MCP-Protocol-Version",
                         std::string(mcp::kProtocolVersion));
        if (_body.contains("method")) {
            _headers.emplace("Mcp-Method",
                             _body.at("method").get<std::string>());
        }
        httplib::Client client("127.0.0.1", server.http_port());
        return client.Post("/mcp", _headers, _body.dump(), "application/json");
    }
};

}
