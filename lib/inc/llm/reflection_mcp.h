#pragma once

// A minimal MCP server base. CRTP: the derived class declares its tools,
// prompts and resources as ordinary (non-static) member functions carrying a
// marker, and the base constructor scans `Derived` by reflection -- no
// registration line, no separate namespace. It owns the JSON-RPC 2.0 loop and
// the three tables. Targets protocol revision 2026-07-28 (stateless) and also
// answers the handshake-era `initialize`, so a client built before that
// revision -- Cursor ships @modelcontextprotocol/sdk 1.25.1, which opens with
// `initialize` and never probes `server/discover` -- can still connect. See
// docs/about_llm.md.

#include "llm/reflection_json.h"

#include <atomic>
#include <cppcodec/base64_rfc4648.hpp>
#include <cstddef>
#include <httplib.h>
#include <iostream>
#include <istream>
#include <meta>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcp {

/// The protocol revision this server speaks.
inline constexpr std::string_view kProtocolVersion = "2026-07-28";

/// The newest revision that still used the `initialize` handshake. Only this
/// one is answered: a client that predates it is told so rather than served,
/// which is also the revision Cursor's bundled client (TS SDK 1.25.1) names.
inline constexpr std::string_view kHandshakeVersion = "2025-11-25";

/// The dialect a connection is answered in. The handshake era predates
/// per-result metadata, so its replies carry none of it: a connection is served
/// in one dialect throughout, never one at the handshake and another after it.
enum class Era {
    /// 2026-07-28. Stateless, and every result identifies the server in
    /// `_meta`.
    modern,
    /// 2025-11-25. Opened by `initialize`, which is all the era defines.
    handshake,
};

/// Marks a member function as a prompt. Name, description, arguments and the
/// call all come from the function, so the marker is the only registration.
struct prompt {};

/// Marks a member function as a tool. Name, description, schema and the call
/// still come from the function. The four fields are `ToolAnnotations`; their
/// defaults are the spec's, so a bare `tool{}` means "may write, may destroy,
/// not safe to retry, may reach outside". `destructive` and `idempotent` apply
/// only when `read_only` is false. They are client hints, not schema fields.
struct tool {
    bool read_only = false;
    bool destructive = true;
    bool idempotent = false;
    bool open_world = true;
};

/// A resource marker, which (unlike tool / prompt) also carries the URI and
/// MIME type as structural strings: the marker is the only place a real
/// `file:///...` URI and a MIME type can live. Name and description still come
/// from reflection.
template <std::size_t U, std::size_t M = 1> struct resource {
    js::str<U> uri;
    js::str<M> mime_type;
};

template <std::size_t U, std::size_t M>
resource(js::str<U>, js::str<M>) -> resource<U, M>;
template <std::size_t U> resource(js::str<U>) -> resource<U, 1>;

// --- reflection helpers -----------------------------------------------------

/// True when `E` carries an annotation of type `Tmpl`, whether `Tmpl` is a
/// plain type (mcp::tool / mcp::prompt) or a template (mcp::resource<...>).
template <std::meta::info E, std::meta::info Tmpl>
consteval bool has_annotation() {
    for (std::meta::info a : std::meta::annotations_of(E)) {
        const std::meta::info t =
            std::meta::remove_const(std::meta::type_of(a));
        if (t == Tmpl || (std::meta::has_template_arguments(t) &&
                          std::meta::template_of(t) == Tmpl)) {
            return true;
        }
    }
    return false;
}

/// members_of() returns a heap-allocated vector, which cannot be bound to a
/// constexpr variable; define_static_array inside a consteval call is the
/// persistent form. The class's members include constructors, the destructor
/// and operators, so the scan filters on is_function && has_identifier.
template <std::meta::info Class> consteval auto class_member_infos() {
    return std::define_static_array(
        std::meta::members_of(Class, std::meta::access_context::unchecked()));
}

template <std::meta::info Class> consteval std::size_t class_member_count() {
    return std::meta::members_of(Class, std::meta::access_context::unchecked())
        .size();
}

/// Materializes the reflected description into static storage. This must happen
/// inside a consteval function: define_static_string cannot copy a std::string
/// that was built at runtime (the mirror of js::function_schema, which does the
/// same for the schema).
template <std::meta::info Fn> consteval std::string_view tool_description() {
    return std::string_view(
        std::define_static_string(js::description_of<Fn>()));
}

/// The tool marker's hints. `js::find_annotation` only matches templates, and
/// `tool` is a plain type, so this walks annotations itself.
template <std::meta::info Fn> consteval std::meta::info tool_annotation() {
    for (std::meta::info a : std::meta::annotations_of(Fn)) {
        if (std::meta::remove_const(std::meta::type_of(a)) == ^^tool) {
            return a;
        }
    }
    return {};
}

template <std::meta::info Fn> consteval tool tool_marker() {
    constexpr std::meta::info anno = tool_annotation<Fn>();
    static_assert(anno != std::meta::info{},
                  "mcp tool needs [[= mcp::tool{}]]");
    return std::meta::extract<tool>(anno);
}

/// The resource marker's consteval-extracted config object.
template <std::meta::info Fn> consteval auto resource_marker() {
    constexpr std::meta::info anno = js::find_annotation<^^mcp::resource>(Fn);
    static_assert(anno != std::meta::info{},
                  "mcp resource needs [[= mcp::resource{...}]]");
    return std::meta::extract<typename[:std::meta::type_of(anno):]>(anno);
}

template <std::meta::info Fn> consteval std::string_view resource_uri() {
    constexpr auto cfg = resource_marker<Fn>();
    return std::string_view(
        std::define_static_string(std::string_view(cfg.uri.data)));
}

template <std::meta::info Fn> consteval std::string_view resource_mime() {
    constexpr auto cfg = resource_marker<Fn>();
    return std::string_view(
        std::define_static_string(std::string_view(cfg.mime_type.data)));
}

/// PromptArgument has no `type`, so a prompt parameter must be a string; an
/// optional string only flips `required`.
template <typename T> struct is_prompt_argument : std::false_type {};
template <> struct is_prompt_argument<std::string> : std::true_type {};
template <>
struct is_prompt_argument<std::optional<std::string>> : std::true_type {};

template <std::meta::info Fn, std::size_t I>
consteval std::string prompt_argument_json() {
    using PT = js::param_type_at<Fn, I>;
    static_assert(is_prompt_argument<PT>::value,
                  "mcp prompt argument must be std::string or "
                  "std::optional<std::string>");
    const std::string name(meta::identifier_of(js::param_at<Fn, I>()));
    const std::string desc = js::param_description<Fn, I>();
    std::string out = "{\"name\":\"" + js::json_escape(name) + "\"";
    if (!desc.empty()) {
        out += ",\"description\":\"" + js::json_escape(desc) + "\"";
    }
    // As with the tool schemas: a default argument cannot be recovered at
    // dispatch time, so it does not make the argument optional.
    const bool required = !js::is_optional<PT>::value;
    out += std::string(",\"required\":") + (required ? "true" : "false") + "}";
    return out;
}

template <std::meta::info Fn, std::size_t... Is>
consteval std::string prompt_arguments_impl(std::index_sequence<Is...>) {
    std::string out = "[";
    std::size_t written = 0;
    ((void)([&] {
         if (written++ != 0) {
             out += ",";
         }
         out += prompt_argument_json<Fn, Is>();
     }()),
     ...);
    out += "]";
    return out;
}

/// The prompt's `arguments` array, materialized for the registry.
template <std::meta::info Fn> consteval std::string_view prompt_arguments() {
    return std::string_view(std::define_static_string(prompt_arguments_impl<Fn>(
        std::make_index_sequence<js::param_count<Fn>()>())));
}

namespace detail {

/// Per-entity trampoline. A pointer-to-member cannot be formed from a runtime
/// reflection, but it can from a template instantiation, so the registry stores
/// `&invoke_member_entry<Fn, Self>` plus the object. A body must return
/// std::string.
template <std::meta::info Fn, typename Self>
std::string invoke_member_entry(void *_self, const nlohmann::json &_args) {
    static_assert(
        std::is_same_v<
            std::remove_cvref_t<decltype(js::invoke_member_with_json<Fn>(
                *static_cast<Self *>(_self), _args))>,
            std::string>,
        "mcp body must return std::string");
    return js::invoke_member_with_json<Fn>(*static_cast<Self *>(_self), _args);
}

/// A JSON-RPC level failure (as opposed to a tool that ran and failed, which is
/// a normal result with `isError`).
struct RpcError : std::runtime_error {
    RpcError(int _code, std::string _message,
             nlohmann::json _data = nlohmann::json())
        : std::runtime_error(std::move(_message)), code(_code),
          data(std::move(_data)) {}
    int code;
    nlohmann::json data;
};

inline nlohmann::json ok(const nlohmann::json &_id, nlohmann::json _result) {
    return nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", _id}, {"result", std::move(_result)}};
}

inline nlohmann::json err(const nlohmann::json &_id, int _code,
                          const std::string &_message,
                          nlohmann::json _data = nlohmann::json()) {
    nlohmann::json error{{"code", _code}, {"message", _message}};
    if (!_data.is_null()) {
        error["data"] = std::move(_data);
    }
    return nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", _id}, {"error", std::move(error)}};
}

inline nlohmann::json text_content(std::string _text, bool _is_error) {
    using nlohmann::json;
    return json{{"resultType", "complete"},
                {"content", json::array({json{{"type", "text"},
                                              {"text", std::move(_text)}}})},
                {"isError", _is_error}};
}

} // namespace detail

// --- the base class ---------------------------------------------------------

template <class Derived> class Server {
public:
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    virtual ~Server();

    /// The protocol loop: newline-delimited JSON-RPC 2.0 over the given
    /// streams. Only protocol JSON is written to `_out`; diagnostics belong on
    /// stderr, so this path must never call the stdout logger. Returns 0 at end
    /// of input.
    int serve(std::istream &_in, std::ostream &_out);

    /// Local use: an agent launches this process and speaks MCP on
    /// stdin/stdout.
    int stdio();

    /// External use: bind 127.0.0.1 and serve Streamable HTTP at `/mcp`. Blocks
    /// until stop_http(). One request at a time. `_port` 0 picks a free port;
    /// http_port() reports the bound one. Returns 0 when stopped, 1 if bind
    /// fails.
    int serve_http(int _port);

    /// Unblocks serve_http(). Safe to call from another thread, and from a
    /// thread that is not serving.
    void stop_http();

    /// The port serve_http() bound, or -1 when it is not listening.
    int http_port() const { return m_http_port.load(); }

    /// Projects the tool table as an OpenAI `tools` array — the very table
    /// MCP's tools/list serves, emitted for the other consumer. Feeds
    /// `--emit-tools`.
    std::string tools_as_openai_json() const;

    /// Runs a named tool for the `--call` CLI, throwing std::runtime_error with
    /// the CLI's exact wording so the Python contract is unchanged.
    std::string call_tool(std::string_view _name,
                          const nlohmann::json &_args) const;

protected:
    /// The whole registration: scan `Derived` and register every marked member.
    Server() { register_members<^^Derived>(); }

    virtual std::string_view server_name() const = 0;
    virtual std::string_view server_version() const = 0;
    virtual std::string_view instructions() const { return {}; }

private:
    struct Tool {
        std::string_view name;
        std::string_view description;
        std::string_view input_schema; // bare JSON Schema, as text
        void *self;
        std::string (*run)(void *, const nlohmann::json &);
        tool hints; // client hints, not schema
    };
    struct Prompt {
        std::string_view name;
        std::string_view description;
        std::string_view arguments; // JSON array of PromptArgument
        void *self;
        std::string (*run)(void *, const nlohmann::json &);
    };
    struct Resource {
        std::string_view uri;
        std::string_view mime_type;
        std::string_view name;
        std::string_view description;
        void *self;
        std::string (*run)(void *, const nlohmann::json &);
    };

    template <std::meta::info M, typename Self>
    void register_member(Self *_self);

    template <std::meta::info Class, typename Self, std::size_t... Is>
    void scan(Self *_self, std::index_sequence<Is...>);

    template <std::meta::info Class> void register_members();

    std::string dispatch(const std::string &_line, Era &_era);
    const Tool *find_tool(std::string_view _name) const;
    nlohmann::json capabilities() const;
    nlohmann::json on_initialize(const nlohmann::json &_params) const;
    nlohmann::json on_discover() const;
    nlohmann::json on_tools_list() const;
    nlohmann::json on_tools_call(const nlohmann::json &_params) const;
    nlohmann::json on_prompts_list() const;
    nlohmann::json on_prompts_get(const nlohmann::json &_params) const;
    nlohmann::json on_resources_list() const;
    nlohmann::json on_resources_read(const nlohmann::json &_params) const;

    std::vector<Tool> m_tools;
    std::vector<Prompt> m_prompts;
    std::vector<Resource> m_resources;

    std::atomic<httplib::Server *> m_http{nullptr};
    std::atomic<int> m_http_port{-1};
    std::atomic<bool> m_http_stop{false};
};

// --- registration -----------------------------------------------------------

template <class Derived>
template <std::meta::info M, typename Self>
void Server<Derived>::register_member(Self *_self) {
    // Constructors, the destructor and operators have no identifier; data
    // members and nested types are not functions. Only marked functions
    // register.
    if constexpr (!std::meta::is_function(M) || !std::meta::has_identifier(M)) {
        return;
    } else if constexpr (has_annotation<M, ^^mcp::tool>()) {
        m_tools.push_back(
            Tool{std::meta::identifier_of(M), tool_description<M>(),
                 js::function_schema<M, js::Style::JsonSchema>(), _self,
                 &detail::invoke_member_entry<M, Self>, tool_marker<M>()});
    } else if constexpr (has_annotation<M, ^^mcp::prompt>()) {
        m_prompts.push_back(Prompt{std::meta::identifier_of(M),
                                   tool_description<M>(), prompt_arguments<M>(),
                                   _self,
                                   &detail::invoke_member_entry<M, Self>});
    } else if constexpr (has_annotation<M, ^^mcp::resource>()) {
        m_resources.push_back(Resource{resource_uri<M>(), resource_mime<M>(),
                                       std::meta::identifier_of(M),
                                       tool_description<M>(), _self,
                                       &detail::invoke_member_entry<M, Self>});
    }
}

template <class Derived>
template <std::meta::info Class, typename Self, std::size_t... Is>
void Server<Derived>::scan(Self *_self, std::index_sequence<Is...>) {
    constexpr auto infos = class_member_infos<Class>();
    (register_member<infos[Is]>(_self), ...);
}

template <class Derived>
template <std::meta::info Class>
void Server<Derived>::register_members() {
    using Self = typename[:Class:];
    scan<Class>(static_cast<Self *>(this),
                std::make_index_sequence<class_member_count<Class>()>{});
}

// --- protocol ---------------------------------------------------------------

template <class Derived>
int Server<Derived>::serve(std::istream &_in, std::ostream &_out) {
    std::string line;
    Era era = Era::modern;
    while (std::getline(_in, line)) {
        if (line.empty()) {
            continue;
        }
        const std::string reply = dispatch(line, era);
        if (!reply.empty()) {
            _out << reply << '\n';
            _out.flush();
        }
    }
    return 0;
}

template <class Derived>
std::string Server<Derived>::tools_as_openai_json() const {
    std::string out = "[";
    for (std::size_t i = 0; i < m_tools.size(); ++i) {
        if (i != 0) {
            out += ",";
        }
        out += js::build_openai_tool(m_tools[i].name, m_tools[i].description,
                                     m_tools[i].input_schema);
    }
    out += "]";
    return out;
}

template <class Derived>
const typename Server<Derived>::Tool *
Server<Derived>::find_tool(std::string_view _name) const {
    for (const Tool &tool : m_tools) {
        if (tool.name == _name) {
            return &tool;
        }
    }
    return nullptr;
}

template <class Derived>
std::string Server<Derived>::call_tool(std::string_view _name,
                                       const nlohmann::json &_args) const {
    const Tool *tool = find_tool(_name);
    if (tool == nullptr) {
        throw std::runtime_error("unknown tool: " + std::string(_name));
    }
    try {
        return tool->run(tool->self, _args);
    } catch (const std::exception &_e) {
        throw std::runtime_error("tool " + std::string(_name) +
                                 " failed: " + _e.what());
    }
}

template <class Derived>
std::string Server<Derived>::dispatch(const std::string &_line, Era &_era) {
    using nlohmann::json;
    json request;
    try {
        request = json::parse(_line);
    } catch (const json::exception &) {
        return detail::err(json(), -32700, "Parse error").dump();
    }
    if (!request.is_object() || request.value("jsonrpc", "") != "2.0" ||
        !request.contains("method")) {
        return detail::err(json(), -32600, "Invalid Request").dump();
    }

    // A request without an id is a notification: process (there is nothing to
    // process yet) and stay silent.
    if (!request.contains("id")) {
        return {};
    }
    const json id = request.at("id");
    const std::string method = request.at("method").get<std::string>();
    const json params = request.value("params", json::object());

    // Per-request protocol version. Lenient when _meta is absent so the server
    // is reachable with a bare JSON-RPC line; a present-but-unknown version is
    // an UnsupportedProtocolVersionError per the revision.
    const std::string version =
        params.value("_meta", json::object())
            .value("io.modelcontextprotocol/protocolVersion", "");
    if (!version.empty() && version != kProtocolVersion) {
        return detail::err(id, -32022, "Unsupported protocol version",
                           json{{"supported", json::array({kProtocolVersion})},
                                {"requested", version}})
            .dump();
    }

    try {
        if (method == "initialize") {
            _era = Era::handshake;
            return detail::ok(id, on_initialize(params)).dump();
        }
        json result;
        if (method == "server/discover") {
            result = on_discover();
        } else if (method == "ping") {
            result = json::object();
        } else if (method == "tools/list") {
            result = on_tools_list();
        } else if (method == "tools/call") {
            result = on_tools_call(params);
        } else if (method == "prompts/list") {
            result = on_prompts_list();
        } else if (method == "prompts/get") {
            result = on_prompts_get(params);
        } else if (method == "resources/list") {
            result = on_resources_list();
        } else if (method == "resources/read") {
            result = on_resources_read(params);
        } else {
            return detail::err(id, -32601, "Method not found: " + method)
                .dump();
        }
        // TODO: Improve this.
        if (_era == Era::handshake) {
            for (const char *field :
                 {"_meta", "resultType", "ttlMs", "cacheScope"}) {
                result.erase(field);
            }
        } else {
            result["_meta"]["io.modelcontextprotocol/serverInfo"] = {
                {"name", std::string(server_name())},
                {"version", std::string(server_version())}};
        }
        return detail::ok(id, std::move(result)).dump();
    } catch (const detail::RpcError &_e) {
        return detail::err(id, _e.code, _e.what(), _e.data).dump();
    } catch (const std::exception &_e) {
        return detail::err(id, -32603, _e.what()).dump();
    }
}

template <class Derived> nlohmann::json Server<Derived>::capabilities() const {
    using nlohmann::json;
    json capabilities = json::object();
    if (!m_tools.empty()) {
        capabilities["tools"] = json::object();
    }
    if (!m_prompts.empty()) {
        capabilities["prompts"] = json::object();
    }
    if (!m_resources.empty()) {
        capabilities["resources"] = json::object();
    }
    return capabilities;
}

/// The handshake-era `initialize`. It answers only the newest revision of that
/// era and corrects anything else to it, which is what the handshake defines
/// and what keeps an older client from speaking a dialect we have not
/// implemented. The reply carries that era's fields and none of the modern
/// result metadata
/// (`resultType` and friends arrive one revision later).
template <class Derived>
nlohmann::json
Server<Derived>::on_initialize(const nlohmann::json &_params) const {
    using nlohmann::json;
    const std::string requested = _params.value("protocolVersion", "");
    return json{
        {"protocolVersion", requested == kHandshakeVersion
                                ? requested
                                : std::string(kHandshakeVersion)},
        {"capabilities", capabilities()},
        {"serverInfo", json{{"name", std::string(server_name())},
                            {"version", std::string(server_version())}}},
        {"instructions", std::string(instructions())},
    };
}

template <class Derived> nlohmann::json Server<Derived>::on_discover() const {
    using nlohmann::json;
    return json{
        {"resultType", "complete"},
        {"supportedVersions", json::array({kProtocolVersion})},
        {"capabilities", capabilities()},
        {"instructions", std::string(instructions())},
        {"ttlMs", 3600000},
        {"cacheScope", "public"},
    };
}

template <class Derived> nlohmann::json Server<Derived>::on_tools_list() const {
    using nlohmann::json;
    json tools = json::array();
    for (const Tool &tool : m_tools) {
        const bool destructive =
            tool.hints.read_only ? false : tool.hints.destructive;
        tools.push_back(json{
            {"name", std::string(tool.name)},
            {"description", std::string(tool.description)},
            {"inputSchema", json::parse(std::string(tool.input_schema))},
            {"annotations", json{{"readOnlyHint", tool.hints.read_only},
                                 {"destructiveHint", destructive},
                                 {"idempotentHint", tool.hints.idempotent},
                                 {"openWorldHint", tool.hints.open_world}}},
        });
    }
    return json{
        {"resultType", "complete"},
        {"tools", std::move(tools)},
        {"ttlMs", 300000},
        {"cacheScope", "public"},
    };
}

template <class Derived>
nlohmann::json
Server<Derived>::on_tools_call(const nlohmann::json &_params) const {
    using nlohmann::json;
    const std::string name = _params.value("name", "");
    const json args = _params.value("arguments", json::object());
    const Tool *tool = find_tool(name);
    if (tool == nullptr) {
        throw detail::RpcError(-32602, "Unknown tool: " + name);
    }
    try {
        return detail::text_content(tool->run(tool->self, args), false);
    } catch (const std::exception &_e) {
        // The tool ran and threw: a *result* with isError, not a JSON-RPC
        // error.
        return detail::text_content(_e.what(), true);
    }
}

template <class Derived>
nlohmann::json Server<Derived>::on_prompts_list() const {
    using nlohmann::json;
    json prompts = json::array();
    for (const Prompt &prompt : m_prompts) {
        prompts.push_back(json{
            {"name", std::string(prompt.name)},
            {"description", std::string(prompt.description)},
            {"arguments", json::parse(std::string(prompt.arguments))},
        });
    }
    return json{
        {"resultType", "complete"},
        {"prompts", std::move(prompts)},
        {"ttlMs", 300000},
        {"cacheScope", "public"},
    };
}

template <class Derived>
nlohmann::json
Server<Derived>::on_prompts_get(const nlohmann::json &_params) const {
    using nlohmann::json;
    const std::string name = _params.value("name", "");
    const json args = _params.value("arguments", json::object());
    for (const Prompt &prompt : m_prompts) {
        if (prompt.name != name) {
            continue;
        }
        json messages;
        try {
            // The body is the messages JSON; a failure to fill a required
            // argument is a client error (-32602), not an internal one.
            messages = json::parse(prompt.run(prompt.self, args));
        } catch (const json::exception &) {
            throw detail::RpcError(-32603,
                                   "prompt produced invalid messages JSON");
        } catch (const std::exception &_e) {
            throw detail::RpcError(-32602, _e.what());
        }
        return json{
            {"resultType", "complete"},
            {"description", std::string(prompt.description)},
            {"messages", std::move(messages)},
        };
    }
    throw detail::RpcError(-32602, "Unknown prompt: " + name);
}

template <class Derived>
nlohmann::json Server<Derived>::on_resources_list() const {
    using nlohmann::json;
    json resources = json::array();
    for (const Resource &resource : m_resources) {
        resources.push_back(json{
            {"uri", std::string(resource.uri)},
            {"name", std::string(resource.name)},
            {"description", std::string(resource.description)},
            {"mimeType", std::string(resource.mime_type)},
        });
    }
    return json{
        {"resultType", "complete"},
        {"resources", std::move(resources)},
        {"ttlMs", 300000},
        {"cacheScope", "public"},
    };
}

template <class Derived>
nlohmann::json
Server<Derived>::on_resources_read(const nlohmann::json &_params) const {
    using nlohmann::json;
    const std::string uri = _params.value("uri", "");
    for (const Resource &resource : m_resources) {
        if (resource.uri != uri) {
            continue;
        }
        std::string text;
        try {
            text = resource.run(resource.self, json::object());
        } catch (const std::exception &_e) {
            throw detail::RpcError(-32603, _e.what());
        }
        return json{
            {"resultType", "complete"},
            {"contents",
             json::array({json{{"uri", std::string(resource.uri)},
                               {"mimeType", std::string(resource.mime_type)},
                               {"text", std::move(text)}}})},
            {"ttlMs", 300000},
            {"cacheScope", "public"},
        };
    }
    // MUST NOT return empty contents for a missing resource: that is an error.
    throw detail::RpcError(-32602, "Resource not found: " + uri);
}

// --- transports -------------------------------------------------------------

template <class Derived> Server<Derived>::~Server() { stop_http(); }

template <class Derived> int Server<Derived>::stdio() {
    return serve(std::cin, std::cout);
}

template <class Derived> void Server<Derived>::stop_http() {
    m_http_stop.store(true);
    if (httplib::Server *http = m_http.load()) {
        http->stop();
    }
}

namespace detail {

/// Plain header text, or the MCP `=?base64?...?=` sentinel. nullopt is
/// malformed. cppcodec's RFC 4648 decoder is the strict one: unpadded or
/// trailing-junk payloads throw rather than decode to a shorter string that
/// could still match.
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

/// Browser clients send Origin. Only this machine's own origins are accepted;
/// a missing Origin is a non-browser agent and is allowed.
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

/// Streamable HTTP requires these headers to match the body. nullopt means they
/// do.
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
    // Mcp-Method is the header the stateless revision introduced, so it is the
    // one a request opts into the strict contract with. A client without it is
    // from the handshake era -- Cursor's bundled client sends the older
    // MCP-Protocol-Version alone and knows nothing of Mcp-Method -- and is
    // served the older way rather than bounced.
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
    const std::string body_version =
        params.value("_meta", json::object())
            .value("io.modelcontextprotocol/protocolVersion", "");
    if (version != body_version) {
        return fail(
            "Header mismatch: MCP-Protocol-Version does not match body");
    }
    std::string name_field;
    if (method == "tools/call" || method == "prompts/get") {
        name_field = "name";
    } else if (method == "resources/read") {
        name_field = "uri";
    }
    if (!name_field.empty()) {
        const std::string header_name = _req.get_header_value("Mcp-Name");
        const std::optional<std::string> decoded =
            decode_mcp_header(header_name);
        const std::string body_name = params.value(name_field, "");
        if (!decoded || decoded->empty() || *decoded != body_name) {
            return fail("Header mismatch: Mcp-Name does not match body");
        }
    }
    return std::nullopt;
}

inline int http_status(std::string_view _body) {
    const nlohmann::json doc = nlohmann::json::parse(_body, nullptr, false);
    if (doc.is_discarded() || !doc.contains("error")) {
        return 200;
    }
    switch (doc.at("error").value("code", 0)) {
    case -32601:
        return 404;
    case -32603:
        return 500;
    default:
        return 400;
    }
}

} // namespace detail

template <class Derived> int Server<Derived>::serve_http(int _port) {
    httplib::Server http;
    // Tools run on this object. One worker keeps calls off each other.
    http.new_task_queue = [] { return new httplib::ThreadPool(1); };

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

    http.Post("/mcp", [this](const httplib::Request &_req,
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
        json doc;
        try {
            doc = json::parse(_req.body);
        } catch (const json::exception &) {
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
        const std::string reply = dispatch(_req.body, era);
        if (reply.empty()) {
            _res.status = 202;
            return;
        }
        _res.status = detail::http_status(reply);
        _res.set_content(reply, "application/json");
    });

    m_http_stop.store(false);
    // bind_to_any_port is the library's port-0 path: it returns the port the
    // kernel assigned. bind_to_port only reports success, so a fixed port is
    // the one the caller asked for.
    const int bound =
        _port == 0 ? http.bind_to_any_port("127.0.0.1")
                   : (http.bind_to_port("127.0.0.1", _port) ? _port : -1);
    if (bound < 0) {
        std::cerr << "mcp: bind 127.0.0.1:" << _port << " failed\n";
        return 1;
    }
    m_http_port.store(bound);
    m_http.store(&http);
    std::cerr << "mcp: http://127.0.0.1:" << bound << "/mcp\n";
    if (m_http_stop.load()) {
        m_http.store(nullptr);
        m_http_port.store(-1);
        return 0;
    }
    http.listen_after_bind();
    m_http.store(nullptr);
    m_http_port.store(-1);
    return 0;
}

} // namespace mcp
