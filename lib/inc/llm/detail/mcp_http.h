#pragma once

#include "mcp_protocol.h"

namespace mcp {

template <class Derived> Server<Derived>::~Server() { stop_http(); }

template <class Derived> void Server<Derived>::stop_http() {
    std::vector<std::stop_source> stops;
    {
        std::lock_guard lock(m_http_mutex);
        if (m_http) {
            m_http->stopping = true;
            stops = m_http->tasks.mark_stopping();
            m_http->http.stop();
        }
    }
    for (auto &stop : stops) {
        stop.request_stop();
    }
}

namespace detail {

inline std::optional<std::string> decode_mcp_header(std::string_view _value) {
    constexpr std::string_view kPrefix = "=?base64?";
    constexpr std::string_view kSuffix = "?=";
    if (_value.size() < kPrefix.size() + kSuffix.size() ||
        !_value.starts_with(kPrefix) || !_value.ends_with(kSuffix)) {
        return std::string(_value);
    }
    const std::string_view payload = _value.substr(
        kPrefix.size(), _value.size() - kPrefix.size() - kSuffix.size());
    try {
        return cppcodec::base64_rfc4648::decode<std::string>(payload.data(),
                                                             payload.size());
    } catch (const cppcodec::parse_error &) {
        return std::nullopt;
    }
}

inline bool origin_allowed(std::string_view _origin) {
    std::string_view rest = _origin;
    if (rest.starts_with("https://")) {
        rest.remove_prefix(8);
    } else if (rest.starts_with("http://")) {
        rest.remove_prefix(7);
    } else {
        return false;
    }
    const auto slash = rest.find('/');
    if (slash != std::string_view::npos) {
        rest = rest.substr(0, slash);
    }
    if (rest.starts_with('[')) {
        const auto end = rest.find(']');
        return end != std::string_view::npos &&
               rest.substr(0, end + 1) == "[::1]";
    }
    const auto colon = rest.rfind(':');
    if (colon != std::string_view::npos) {
        rest = rest.substr(0, colon);
    }
    return rest == "127.0.0.1" || rest == "localhost";
}

inline bool forbid_origin(const httplib::Request &_req,
                          httplib::Response &_res) {
    const std::string origin = _req.get_header_value("Origin");
    if (origin.empty() || origin_allowed(origin)) {
        return false;
    }
    _res.status = 403;
    _res.set_content(err(nlohmann::json(), -32600, "Forbidden origin").dump(),
                     "application/json");
    return true;
}

inline std::optional<std::string> header_mismatch(const httplib::Request &_req,
                                                  const nlohmann::json &_doc) {
    using nlohmann::json;
    const json id = _doc.contains("id") ? _doc.at("id") : json();
    const auto fail = [&](const std::string &_message) {
        return err(id, -32020, _message).dump();
    };
    if (!_doc.is_object() || !_doc.contains("method") ||
        !_doc.at("method").is_string()) {
        return fail("Header mismatch: required header missing");
    }
    const std::string method = _doc.at("method").get<std::string>();
    const std::string method_header = _req.get_header_value("Mcp-Method");
    if (method_header.empty()) {
        return std::nullopt;
    }
    const std::string version = _req.get_header_value("MCP-Protocol-Version");
    if (version.empty()) {
        return fail("Header mismatch: required header missing");
    }
    if (method_header != method) {
        return fail("Header mismatch: Mcp-Method does not match body");
    }
    const json params = _doc.contains("params") && _doc.at("params").is_object()
                            ? _doc.at("params")
                            : json::object();
    const json::json_pointer body_version(
        "/_meta/io.modelcontextprotocol~1protocolVersion");
    if (!params.contains(body_version) ||
        !params.at(body_version).is_string() ||
        version != params.at(body_version).get_ref<const std::string &>()) {
        return fail(
            "Header mismatch: MCP-Protocol-Version does not match body");
    }
    std::string name_field;
    if (method == "tools/call" || method == "prompts/get") {
        name_field = "name";
    } else if (method == "resources/read") {
        name_field = "uri";
    } else if (method == "tasks/get" || method == "tasks/cancel" ||
               method == "tasks/update") {
        name_field = "taskId";
    }
    if (!name_field.empty()) {
        const std::string header_name = _req.get_header_value("Mcp-Name");
        const std::optional<std::string> decoded =
            decode_mcp_header(header_name);
        const auto body_name = params.find(name_field);
        if (!decoded || decoded->empty() || body_name == params.end() ||
            !body_name->is_string() ||
            *decoded != body_name->get_ref<const std::string &>()) {
            return fail("Header mismatch: Mcp-Name does not match body");
        }
    }
    return std::nullopt;
}

inline int http_status(std::optional<int> _error_code) {
    if (!_error_code) {
        return 200;
    }
    switch (*_error_code) {
    case -32000:
        return 503;
    case -32601:
        return 404;
    case -32603:
        return 500;
    default:
        return 400;
    }
}

}

template <class Derived>
int Server<Derived>::serve_http(std::string_view _host, int _port) {
    const std::string host(_host);
    const std::string authority =
        host.find(':') == std::string::npos ? host : "[" + host + "]";
    HttpRun run(m_workers);
    auto &http = run.http;
    http.new_task_queue = [] {
        constexpr std::size_t http_workers = 2 * kMaxSynchronousCalls;
        return new httplib::ThreadPool(http_workers, http_workers, 64);
    };
    http.set_keep_alive_max_count(1);
    http.set_payload_max_length(256 * 1024);
    http.set_read_timeout(5, 0);
    http.set_write_timeout(5, 0);

    const auto method_not_allowed = [](const httplib::Request &_req,
                                       httplib::Response &_res) {
        if (detail::forbid_origin(_req, _res)) {
            return;
        }
        _res.status = 405;
        _res.set_header("Allow", "POST");
    };
    http.Get("/mcp", method_not_allowed);
    http.Delete("/mcp", method_not_allowed);

    http.Post("/mcp", [this, &run, info = snapshot_server_info()](
                          const httplib::Request &_req,
                          httplib::Response &_res) {
        using nlohmann::json;
        if (detail::forbid_origin(_req, _res)) {
            return;
        }
        const std::string accept = httplib::detail::case_ignore::to_lower(
            _req.get_header_value("Accept"));
        if (accept.find("application/json") == std::string::npos ||
            accept.find("text/event-stream") == std::string::npos) {
            _res.status = 406;
            _res.set_content(
                detail::err(json(), -32600, "Not Acceptable").dump(),
                "application/json");
            return;
        }
        const std::string content_type = httplib::detail::case_ignore::to_lower(
            _req.get_header_value("Content-Type"));
        if (!content_type.starts_with("application/json")) {
            _res.status = 415;
            _res.set_content(
                detail::err(json(), -32600, "Unsupported Media Type").dump(),
                "application/json");
            return;
        }
        const json doc = json::parse(_req.body, nullptr, false);
        if (doc.is_discarded()) {
            _res.status = 400;
            _res.set_content(detail::err(json(), -32700, "Parse error").dump(),
                             "application/json");
            return;
        }
        if (const std::optional<std::string> mismatch =
                detail::header_mismatch(_req, doc)) {
            _res.status = 400;
            _res.set_content(*mismatch, "application/json");
            return;
        }
        Era era = _req.get_header_value("Mcp-Method").empty() ? Era::handshake
                                                              : Era::modern;
        const auto reply =
            run.stopping ? detail::error_reply(doc.contains("id") ? doc.at("id")
                                                                  : json(),
                                               -32000, "MCP server is stopping")
                         : dispatch(doc, era, run.tasks, info);
        if (reply.body.empty()) {
            _res.status = 202;
            return;
        }
        _res.status = detail::http_status(reply.error_code);
        if (_res.status == 503) {
            _res.set_header("Retry-After", "1");
        }
        _res.set_content(reply.body, "application/json");
    });

    int bound;
    {
        std::lock_guard lock(m_http_mutex);
        if (m_http) {
            return 1;
        }
        bound = _port == 0 ? http.bind_to_any_port(host)
                           : (http.bind_to_port(host, _port) ? _port : -1);
        if (bound >= 0) {
            m_http = &run;
            m_http_port.store(bound);
        }
    }
    if (bound < 0) {
        std::cerr << "mcp: bind " << authority << ':' << _port << " failed\n";
        return 1;
    }
    struct Unpublish {
        Server &server;
        ~Unpublish() {
            std::lock_guard lock(server.m_http_mutex);
            server.m_http = nullptr;
            server.m_http_port.store(-1);
        }
    } publication{*this};
    std::cerr << "mcp: http://" << authority << ':' << bound << "/mcp\n";
    const bool served = http.listen_after_bind();
    std::lock_guard lock(m_http_mutex);
    return served || run.stopping ? 0 : 1;
}

}
