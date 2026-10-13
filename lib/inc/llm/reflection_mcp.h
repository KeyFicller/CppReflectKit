#pragma once

// A minimal MCP server base. CRTP: the derived class declares its tools, prompts
// and resources as ordinary (non-static) member functions carrying a marker, and
// the base constructor scans `Derived` by reflection -- no registration line, no
// separate namespace. It owns the JSON-RPC 2.0 loop and the three tables.
// Targets protocol revision 2026-07-28 (stateless; no initialize handshake). See
// docs/about_llm.md.

#include "llm/reflection_json.h"

#include <cstddef>
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

/// A resource marker, which (unlike tool / prompt) also carries the URI and MIME
/// type as structural strings: the marker is the only place a real
/// `file:///...` URI and a MIME type can live. Name and description still come
/// from reflection.
template <std::size_t U, std::size_t M = 1>
struct resource {
    js::str<U> uri;
    js::str<M> mime_type;
};

template <std::size_t U, std::size_t M>
resource(js::str<U>, js::str<M>) -> resource<U, M>;
template <std::size_t U>
resource(js::str<U>) -> resource<U, 1>;

// --- reflection helpers -----------------------------------------------------

/// True when `E` carries an annotation of type `Tmpl`, whether `Tmpl` is a plain
/// type (mcp::tool / mcp::prompt) or a template (mcp::resource<...>).
template <std::meta::info E, std::meta::info Tmpl>
consteval bool has_annotation()
{
    for (std::meta::info a : std::meta::annotations_of(E)) {
        const std::meta::info t = std::meta::remove_const(std::meta::type_of(a));
        if (t == Tmpl ||
            (std::meta::has_template_arguments(t) && std::meta::template_of(t) == Tmpl)) {
            return true;
        }
    }
    return false;
}

/// members_of() returns a heap-allocated vector, which cannot be bound to a
/// constexpr variable; define_static_array inside a consteval call is the
/// persistent form. The class's members include constructors, the destructor and
/// operators, so the scan filters on is_function && has_identifier.
template <std::meta::info Class>
consteval auto class_member_infos()
{
    return std::define_static_array(
        std::meta::members_of(Class, std::meta::access_context::unchecked()));
}

template <std::meta::info Class>
consteval std::size_t class_member_count()
{
    return std::meta::members_of(Class, std::meta::access_context::unchecked()).size();
}

/// Materializes the reflected description into static storage. This must happen
/// inside a consteval function: define_static_string cannot copy a std::string
/// that was built at runtime (the mirror of js::function_schema, which does the
/// same for the schema).
template <std::meta::info Fn>
consteval std::string_view tool_description()
{
    return std::string_view(std::define_static_string(js::description_of<Fn>()));
}

/// The tool marker's hints. `js::find_annotation` only matches templates, and
/// `tool` is a plain type, so this walks annotations itself.
template <std::meta::info Fn>
consteval std::meta::info tool_annotation()
{
    for (std::meta::info a : std::meta::annotations_of(Fn)) {
        if (std::meta::remove_const(std::meta::type_of(a)) == ^^tool) {
            return a;
        }
    }
    return {};
}

template <std::meta::info Fn>
consteval tool tool_marker()
{
    constexpr std::meta::info anno = tool_annotation<Fn>();
    static_assert(anno != std::meta::info{}, "mcp tool needs [[= mcp::tool{}]]");
    return std::meta::extract<tool>(anno);
}

/// The resource marker's consteval-extracted config object.
template <std::meta::info Fn>
consteval auto resource_marker()
{
    constexpr std::meta::info anno = js::find_annotation<^^mcp::resource>(Fn);
    static_assert(anno != std::meta::info{}, "mcp resource needs [[= mcp::resource{...}]]");
    return std::meta::extract<typename [: std::meta::type_of(anno) :]>(anno);
}

template <std::meta::info Fn>
consteval std::string_view resource_uri()
{
    constexpr auto cfg = resource_marker<Fn>();
    return std::string_view(std::define_static_string(std::string_view(cfg.uri.data)));
}

template <std::meta::info Fn>
consteval std::string_view resource_mime()
{
    constexpr auto cfg = resource_marker<Fn>();
    return std::string_view(std::define_static_string(std::string_view(cfg.mime_type.data)));
}

/// PromptArgument has no `type`, so a prompt parameter must be a string; an
/// optional string only flips `required`.
template <typename T>
struct is_prompt_argument : std::false_type {};
template <>
struct is_prompt_argument<std::string> : std::true_type {};
template <>
struct is_prompt_argument<std::optional<std::string>> : std::true_type {};

template <std::meta::info Fn, std::size_t I>
consteval std::string prompt_argument_json()
{
    using PT = js::param_type_at<Fn, I>;
    static_assert(is_prompt_argument<PT>::value,
                  "mcp prompt argument must be std::string or std::optional<std::string>");
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
consteval std::string prompt_arguments_impl(std::index_sequence<Is...>)
{
    std::string out = "[";
    std::size_t written = 0;
    ((void)([&] {
        if (written++ != 0) {
            out += ",";
        }
        out += prompt_argument_json<Fn, Is>();
    }()), ...);
    out += "]";
    return out;
}

/// The prompt's `arguments` array, materialized for the registry.
template <std::meta::info Fn>
consteval std::string_view prompt_arguments()
{
    return std::string_view(std::define_static_string(
        prompt_arguments_impl<Fn>(std::make_index_sequence<js::param_count<Fn>()>())));
}

namespace detail {

/// Per-entity trampoline. A pointer-to-member cannot be formed from a runtime
/// reflection, but it can from a template instantiation, so the registry stores
/// `&invoke_member_entry<Fn, Self>` plus the object. A body must return
/// std::string.
template <std::meta::info Fn, typename Self>
std::string invoke_member_entry(void* _self, const nlohmann::json& _args)
{
    static_assert(std::is_same_v<std::remove_cvref_t<decltype(js::invoke_member_with_json<Fn>(
                                     *static_cast<Self*>(_self), _args))>,
                                 std::string>,
                  "mcp body must return std::string");
    return js::invoke_member_with_json<Fn>(*static_cast<Self*>(_self), _args);
}

/// A JSON-RPC level failure (as opposed to a tool that ran and failed, which is a
/// normal result with `isError`).
struct RpcError : std::runtime_error {
    RpcError(int _code, std::string _message, nlohmann::json _data = nlohmann::json())
        : std::runtime_error(std::move(_message)), code(_code), data(std::move(_data))
    {
    }
    int code;
    nlohmann::json data;
};

inline nlohmann::json ok(const nlohmann::json& _id, nlohmann::json _result)
{
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", _id}, {"result", std::move(_result)}};
}

inline nlohmann::json err(const nlohmann::json& _id, int _code, const std::string& _message,
                          nlohmann::json _data = nlohmann::json())
{
    nlohmann::json error{{"code", _code}, {"message", _message}};
    if (!_data.is_null()) {
        error["data"] = std::move(_data);
    }
    return nlohmann::json{{"jsonrpc", "2.0"}, {"id", _id}, {"error", std::move(error)}};
}

inline nlohmann::json text_content(std::string _text, bool _is_error)
{
    using nlohmann::json;
    return json{{"resultType", "complete"},
                {"content", json::array({json{{"type", "text"}, {"text", std::move(_text)}}})},
                {"isError", _is_error}};
}

} // namespace detail

// --- the base class ---------------------------------------------------------

template <class Derived>
class Server {
public:
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    virtual ~Server() = default;

    /// The protocol loop: newline-delimited JSON-RPC 2.0 over the given streams.
    /// Only protocol JSON is written to `_out`; diagnostics belong on stderr, so
    /// this path must never call the stdout logger. Returns 0 at end of input.
    int serve(std::istream& _in, std::ostream& _out);

    /// Projects the tool table as an OpenAI `tools` array — the very table MCP's
    /// tools/list serves, emitted for the other consumer. Feeds `--emit-tools`.
    std::string tools_as_openai_json() const;

    /// Runs a named tool for the `--call` CLI, throwing std::runtime_error with
    /// the CLI's exact wording so the Python contract is unchanged.
    std::string call_tool(std::string_view _name, const nlohmann::json& _args) const;

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
        std::string_view input_schema;                 // bare JSON Schema, as text
        void* self;
        std::string (*run)(void*, const nlohmann::json&);
        tool hints;                                    // client hints, not schema
    };
    struct Prompt {
        std::string_view name;
        std::string_view description;
        std::string_view arguments;                    // JSON array of PromptArgument
        void* self;
        std::string (*run)(void*, const nlohmann::json&);
    };
    struct Resource {
        std::string_view uri;
        std::string_view mime_type;
        std::string_view name;
        std::string_view description;
        void* self;
        std::string (*run)(void*, const nlohmann::json&);
    };

    template <std::meta::info M, typename Self>
    void register_member(Self* _self);

    template <std::meta::info Class, typename Self, std::size_t... Is>
    void scan(Self* _self, std::index_sequence<Is...>);

    template <std::meta::info Class>
    void register_members();

    std::string dispatch(const std::string& _line);
    const Tool* find_tool(std::string_view _name) const;
    nlohmann::json on_discover() const;
    nlohmann::json on_tools_list() const;
    nlohmann::json on_tools_call(const nlohmann::json& _params) const;
    nlohmann::json on_prompts_list() const;
    nlohmann::json on_prompts_get(const nlohmann::json& _params) const;
    nlohmann::json on_resources_list() const;
    nlohmann::json on_resources_read(const nlohmann::json& _params) const;

    std::vector<Tool> m_tools;
    std::vector<Prompt> m_prompts;
    std::vector<Resource> m_resources;
};

// --- registration -----------------------------------------------------------

template <class Derived>
template <std::meta::info M, typename Self>
void Server<Derived>::register_member(Self* _self)
{
    // Constructors, the destructor and operators have no identifier; data
    // members and nested types are not functions. Only marked functions register.
    if constexpr (!std::meta::is_function(M) || !std::meta::has_identifier(M)) {
        return;
    } else if constexpr (has_annotation<M, ^^mcp::tool>()) {
        m_tools.push_back(Tool{std::meta::identifier_of(M), tool_description<M>(),
                               js::function_schema<M, js::Style::JsonSchema>(), _self,
                               &detail::invoke_member_entry<M, Self>, tool_marker<M>()});
    } else if constexpr (has_annotation<M, ^^mcp::prompt>()) {
        m_prompts.push_back(Prompt{std::meta::identifier_of(M), tool_description<M>(),
                                   prompt_arguments<M>(), _self,
                                   &detail::invoke_member_entry<M, Self>});
    } else if constexpr (has_annotation<M, ^^mcp::resource>()) {
        m_resources.push_back(Resource{resource_uri<M>(), resource_mime<M>(),
                                       std::meta::identifier_of(M), tool_description<M>(), _self,
                                       &detail::invoke_member_entry<M, Self>});
    }
}

template <class Derived>
template <std::meta::info Class, typename Self, std::size_t... Is>
void Server<Derived>::scan(Self* _self, std::index_sequence<Is...>)
{
    constexpr auto infos = class_member_infos<Class>();
    (register_member<infos[Is]>(_self), ...);
}

template <class Derived>
template <std::meta::info Class>
void Server<Derived>::register_members()
{
    using Self = typename [: Class :];
    scan<Class>(static_cast<Self*>(this), std::make_index_sequence<class_member_count<Class>()>{});
}

// --- protocol ---------------------------------------------------------------

template <class Derived>
int Server<Derived>::serve(std::istream& _in, std::ostream& _out)
{
    std::string line;
    while (std::getline(_in, line)) {
        if (line.empty()) {
            continue;
        }
        const std::string reply = dispatch(line);
        if (!reply.empty()) {
            _out << reply << '\n';
            _out.flush();
        }
    }
    return 0;
}

template <class Derived>
std::string Server<Derived>::tools_as_openai_json() const
{
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
const typename Server<Derived>::Tool* Server<Derived>::find_tool(std::string_view _name) const
{
    for (const Tool& tool : m_tools) {
        if (tool.name == _name) {
            return &tool;
        }
    }
    return nullptr;
}

template <class Derived>
std::string Server<Derived>::call_tool(std::string_view _name, const nlohmann::json& _args) const
{
    const Tool* tool = find_tool(_name);
    if (tool == nullptr) {
        throw std::runtime_error("unknown tool: " + std::string(_name));
    }
    try {
        return tool->run(tool->self, _args);
    } catch (const std::exception& _e) {
        throw std::runtime_error("tool " + std::string(_name) + " failed: " + _e.what());
    }
}

template <class Derived>
std::string Server<Derived>::dispatch(const std::string& _line)
{
    using nlohmann::json;
    json request;
    try {
        request = json::parse(_line);
    } catch (const json::exception&) {
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

    // Per-request protocol version. Lenient when _meta is absent so the server is
    // reachable with a bare JSON-RPC line; a present-but-unknown version is an
    // UnsupportedProtocolVersionError per the revision.
    const std::string version = params.value("_meta", json::object())
                                    .value("io.modelcontextprotocol/protocolVersion", "");
    if (!version.empty() && version != kProtocolVersion) {
        return detail::err(id, -32022, "Unsupported protocol version",
                           json{{"supported", json::array({kProtocolVersion})},
                                {"requested", version}})
            .dump();
    }

    try {
        json result;
        if (method == "server/discover") {
            result = on_discover();
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
            return detail::err(id, -32601, "Method not found: " + method).dump();
        }
        // Stateless: every result identifies the server. Errors have no result.
        result["_meta"]["io.modelcontextprotocol/serverInfo"] = {
            {"name", std::string(server_name())}, {"version", std::string(server_version())}};
        return detail::ok(id, std::move(result)).dump();
    } catch (const detail::RpcError& _e) {
        return detail::err(id, _e.code, _e.what(), _e.data).dump();
    } catch (const std::exception& _e) {
        return detail::err(id, -32603, _e.what()).dump();
    }
}

template <class Derived>
nlohmann::json Server<Derived>::on_discover() const
{
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
    return json{
        {"resultType", "complete"},
        {"supportedVersions", json::array({kProtocolVersion})},
        {"capabilities", std::move(capabilities)},
        {"instructions", std::string(instructions())},
        {"ttlMs", 3600000},
        {"cacheScope", "public"},
    };
}

template <class Derived>
nlohmann::json Server<Derived>::on_tools_list() const
{
    using nlohmann::json;
    json tools = json::array();
    for (const Tool& tool : m_tools) {
        tools.push_back(json{
            {"name", std::string(tool.name)},
            {"description", std::string(tool.description)},
            {"inputSchema", json::parse(std::string(tool.input_schema))},
            {"annotations",
             json{{"readOnlyHint", tool.hints.read_only},
                  {"destructiveHint", tool.hints.destructive},
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
nlohmann::json Server<Derived>::on_tools_call(const nlohmann::json& _params) const
{
    using nlohmann::json;
    const std::string name = _params.value("name", "");
    const json args = _params.value("arguments", json::object());
    const Tool* tool = find_tool(name);
    if (tool == nullptr) {
        throw detail::RpcError(-32602, "Unknown tool: " + name);
    }
    try {
        return detail::text_content(tool->run(tool->self, args), false);
    } catch (const std::exception& _e) {
        // The tool ran and threw: a *result* with isError, not a JSON-RPC error.
        return detail::text_content(_e.what(), true);
    }
}

template <class Derived>
nlohmann::json Server<Derived>::on_prompts_list() const
{
    using nlohmann::json;
    json prompts = json::array();
    for (const Prompt& prompt : m_prompts) {
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
nlohmann::json Server<Derived>::on_prompts_get(const nlohmann::json& _params) const
{
    using nlohmann::json;
    const std::string name = _params.value("name", "");
    const json args = _params.value("arguments", json::object());
    for (const Prompt& prompt : m_prompts) {
        if (prompt.name != name) {
            continue;
        }
        json messages;
        try {
            // The body is the messages JSON; a failure to fill a required
            // argument is a client error (-32602), not an internal one.
            messages = json::parse(prompt.run(prompt.self, args));
        } catch (const json::exception&) {
            throw detail::RpcError(-32603, "prompt produced invalid messages JSON");
        } catch (const std::exception& _e) {
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
nlohmann::json Server<Derived>::on_resources_list() const
{
    using nlohmann::json;
    json resources = json::array();
    for (const Resource& resource : m_resources) {
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
nlohmann::json Server<Derived>::on_resources_read(const nlohmann::json& _params) const
{
    using nlohmann::json;
    const std::string uri = _params.value("uri", "");
    for (const Resource& resource : m_resources) {
        if (resource.uri != uri) {
            continue;
        }
        std::string text;
        try {
            text = resource.run(resource.self, json::object());
        } catch (const std::exception& _e) {
            throw detail::RpcError(-32603, _e.what());
        }
        return json{
            {"resultType", "complete"},
            {"contents", json::array({json{
                {"uri", std::string(resource.uri)},
                {"mimeType", std::string(resource.mime_type)},
                {"text", std::move(text)}}})},
            {"ttlMs", 300000},
            {"cacheScope", "public"},
        };
    }
    // MUST NOT return empty contents for a missing resource: that is an error.
    throw detail::RpcError(-32602, "Resource not found: " + uri);
}

} // namespace mcp
