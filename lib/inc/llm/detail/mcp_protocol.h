#pragma once

#include "mcp_server.h"

#include <algorithm>
#include <iostream>
#include <optional>

namespace mcp {
namespace detail {

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

struct RpcReply {
    std::string body;
    std::optional<int> error_code{};
};

inline RpcReply error_reply(const nlohmann::json &_id, int _code,
                            const std::string &_message,
                            nlohmann::json _data = nlohmann::json()) {
    return {err(_id, _code, _message, std::move(_data)).dump(), _code};
}

inline bool declares_tasks(const nlohmann::json &_params) {
    const nlohmann::json::json_pointer extension(
        "/_meta/io.modelcontextprotocol~1clientCapabilities/extensions/"
        "io.modelcontextprotocol~1tasks");
    return _params.contains(extension) && _params.at(extension).is_object();
}

inline nlohmann::json task_operation(std::string_view _method,
                                     const nlohmann::json &_params,
                                     TaskRuntime &_tasks) {
    using nlohmann::json;
    if (!declares_tasks(_params)) {
        throw RpcError(
            -32021, "Missing required client capability",
            {{"requiredCapabilities",
              {{"extensions", {{kTasksExtension, json::object()}}}}}});
    }
    const auto task_id = _params.find("taskId");
    if (task_id == _params.end() || !task_id->is_string() ||
        task_id->get_ref<const std::string &>().empty()) {
        throw RpcError(-32602, "Expected a non-empty taskId");
    }
    const auto &key = task_id->get_ref<const std::string &>();
    const json complete{{"resultType", "complete"}};
    if (_method == "tasks/get") {
        json result = _tasks.get(key);
        if (!result.is_null()) {
            return result;
        }
    } else if (_method == "tasks/cancel") {
        if (_tasks.cancel(key)) {
            return complete;
        }
    } else {
        const auto responses = _params.find("inputResponses");
        if (responses == _params.end() || !responses->is_object()) {
            throw RpcError(-32602, "Expected an inputResponses object");
        }
        if (_tasks.contains(key)) {
            return complete;
        }
    }
    throw RpcError(-32602, "Unknown taskId");
}

template <typename T> consteval bool is_flat_form() {
    if constexpr (!std::is_class_v<T>) {
        return false;
    } else {
        for (std::meta::info member : std::meta::nonstatic_data_members_of(
                 ^^T, std::meta::access_context::unchecked())) {
            const std::meta::info type =
                std::meta::dealias(std::meta::type_of(member));
            if (!std::meta::is_integral_type(type) &&
                !std::meta::is_floating_point_type(type) &&
                !std::meta::is_same_type(type, ^^std::string)) {
                return false;
            }
        }
        return true;
    }
}

template <typename T> nlohmann::json flat_schema() {
    using nlohmann::json;
    json properties = json::object();
    json required = json::array();
    constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()));
    template for (constexpr std::meta::info member : members) {
        const std::string name(std::meta::identifier_of(member));
        properties[name] = {
            {"type", js::detail::json_type_name(std::meta::type_of(member))}};
        required.push_back(name);
    }
    return json{{"type", "object"},
                {"properties", std::move(properties)},
                {"required", std::move(required)}};
}

struct ElicitAsk {
    nlohmann::json request;
};

}

template <class Derived>
detail::CallContext *Server<Derived>::current_call() const {
    std::lock_guard lock(m_calls_mutex);
    const auto it = m_calls.find(std::this_thread::get_id());
    return it == m_calls.end() ? nullptr : it->second;
}

template <class Derived> bool Server<Derived>::task_stop_requested() const {
    const auto *context = current_call();
    return context && context->task.stop.stop_requested();
}

template <class Derived>
void Server<Derived>::report_task_progress(std::string_view _message) const {
    if (const auto *context = current_call()) {
        context->task.report(_message);
    }
}

template <class Derived>
std::unique_lock<std::mutex> Server<Derived>::lock_inline_call() const {
    std::unique_lock lock(m_inline_mutex, std::try_to_lock);
    if (!lock) {
        throw detail::TaskUnavailable(
            "A synchronous MCP call is already running");
    }
    return lock;
}

template <class Derived>
template <typename T>
elicit_result<T> Server<Derived>::elicit_form(std::string _message) const {
    static_assert(detail::is_flat_form<T>(),
                  "elicit_form needs a struct of bool, integer, floating point "
                  "and std::string members");
    using nlohmann::json;
    const elicit_result<T> unavailable{elicit_action::unavailable,
                                       std::nullopt};
    detail::CallContext *scope = current_call();
    if (scope == nullptr || scope->params == nullptr || scope->asked) {
        return unavailable;
    }
    scope->asked = true;
    const json &params = *scope->params;

    if (params.contains("inputResponses")) {
        const json::json_pointer path("/inputResponses/elicit/action");
        if (!params.contains(path) || !params.at(path).is_string()) {
            return unavailable;
        }
        const json &answer = params.at("inputResponses").at("elicit");
        const std::string action = answer.at("action").get<std::string>();
        if (action == "decline") {
            return {elicit_action::decline, std::nullopt};
        }
        if (action == "cancel") {
            return {elicit_action::cancel, std::nullopt};
        }
        if (action != "accept" || !answer.contains("content") ||
            !answer.at("content").is_object()) {
            return unavailable;
        }
        try {
            return {elicit_action::accept,
                    js::from_json<T>(answer.at("content"))};
        } catch (const std::exception &) {
            return unavailable;
        }
    }

    const json::json_pointer form(
        "/_meta/io.modelcontextprotocol~1clientCapabilities/elicitation/form");
    if (!params.contains(form) || params.at(form).is_null()) {
        return unavailable;
    }
    throw detail::ElicitAsk{
        json{{"method", "elicitation/create"},
             {"params",
              {{"mode", "form"},
               {"message", std::move(_message)},
               {"requestedSchema", detail::flat_schema<T>()}}}}};
}

template <class Derived>
typename Server<Derived>::ServerInfo
Server<Derived>::snapshot_server_info() const {
    return {{{"name", std::string(server_name())},
             {"version", std::string(server_version())}},
            std::string(instructions())};
}

template <class Derived> int Server<Derived>::stdio() {
    return serve(std::cin, std::cout);
}

template <class Derived>
int Server<Derived>::serve(std::istream &_in, std::ostream &_out) {
    std::string line;
    Era era = Era::modern;
    const ServerInfo info = snapshot_server_info();
    detail::TaskRuntime tasks(m_workers, true);
    while (std::getline(_in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto request = nlohmann::json::parse(line, nullptr, false);
        const auto reply = dispatch(request, era, tasks, info);
        if (!reply.body.empty()) {
            _out << reply.body << '\n';
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
        out += js::detail::build_openai_tool(
            m_tools[i].name, m_tools[i].description, m_tools[i].input_schema);
    }
    out += "]";
    return out;
}

template <class Derived>
const typename Server<Derived>::Tool *
Server<Derived>::find_tool(std::string_view _name) const {
    const auto tool = std::ranges::find(m_tools, _name, &Tool::name);
    return tool == m_tools.end() ? nullptr : &*tool;
}

template <class Derived>
std::string Server<Derived>::call_tool(std::string_view _name,
                                       const nlohmann::json &_args) const {
    const Tool *tool = find_tool(_name);
    if (tool == nullptr) {
        throw std::runtime_error("unknown tool: " + std::string(_name));
    }
    try {
        const auto result = tool->run(tool->self, _args);
        std::string output;
        for (const auto &block : result.at("content")) {
            if (block.at("type") == "text") {
                output +=
                    block.at("text").template get_ref<const std::string &>();
            }
        }
        return output;
    } catch (const std::exception &_e) {
        throw std::runtime_error("tool " + std::string(_name) +
                                 " failed: " + _e.what());
    }
}

template <class Derived>
detail::RpcReply Server<Derived>::dispatch(const nlohmann::json &request,
                                           Era &_era,
                                           detail::TaskRuntime &_tasks,
                                           const ServerInfo &_info) {
    using nlohmann::json;
    if (request.is_discarded()) {
        return detail::error_reply(json(), -32700, "Parse error");
    }
    if (!request.is_object() || !request.contains("jsonrpc") ||
        request.at("jsonrpc") != "2.0" || !request.contains("method") ||
        !request.at("method").is_string()) {
        return detail::error_reply(json(), -32600, "Invalid Request");
    }

    if (!request.contains("id")) {
        return {};
    }
    const json id = request.at("id");
    const std::string method = request.at("method").get<std::string>();
    const json params = request.value("params", json::object());
    if (!params.is_object()) {
        return detail::error_reply(id, -32602, "Expected a params object");
    }

    const json::json_pointer protocol_version(
        "/_meta/io.modelcontextprotocol~1protocolVersion");
    const std::string version =
        params.contains(protocol_version) &&
                params.at(protocol_version).is_string()
            ? params.at(protocol_version).get<std::string>()
            : std::string();
    if (!version.empty() && version != kProtocolVersion) {
        return detail::error_reply(
            id, -32022, "Unsupported protocol version",
            json{{"supported", json::array({kProtocolVersion})},
                 {"requested", version}});
    }

    try {
        if (method == "initialize") {
            _era = Era::handshake;
            return {detail::ok(id, on_initialize(params, _info)).dump()};
        }
        std::optional<ActiveCall> scope;
        if (_era == Era::modern &&
            (method == "tools/call" || method == "prompts/get" ||
             method == "resources/read")) {
            scope.emplace(*this, detail::CallContext{.params = &params});
        }
        json result;
        try {
            auto handled = handle_method(method, params, _era, _tasks, _info);
            if (!handled) {
                return detail::error_reply(id, -32601,
                                           "Method not found: " + method);
            }
            result = std::move(*handled);
        } catch (const detail::ElicitAsk &_ask) {
            result = json{{"resultType", "input_required"},
                          {"inputRequests", {{"elicit", _ask.request}}}};
        }
        if (_era == Era::handshake) {
            for (const char *field :
                 {"_meta", "resultType", "ttlMs", "cacheScope"}) {
                result.erase(field);
            }
        } else {
            result["_meta"]["io.modelcontextprotocol/serverInfo"] =
                _info.identity;
        }
        return {detail::ok(id, std::move(result)).dump()};
    } catch (const detail::TaskUnavailable &_e) {
        return detail::error_reply(id, -32000, _e.what());
    } catch (const detail::RpcError &_e) {
        return detail::error_reply(id, _e.code, _e.what(), _e.data);
    } catch (const std::exception &_e) {
        return detail::error_reply(id, -32603, _e.what());
    }
}

template <class Derived>
std::optional<nlohmann::json> Server<Derived>::handle_method(
    std::string_view _method, const nlohmann::json &_params, Era _era,
    detail::TaskRuntime &_tasks, const ServerInfo &_info) const {
    if (_method == "server/discover") {
        return on_discover(_info);
    }
    if (_method == "ping") {
        return nlohmann::json::object();
    }
    if (_method == "tools/list") {
        return on_tools_list();
    }
    if (_method == "tools/call") {
        return on_tools_call(_params, _era, _tasks);
    }
    if (_era == Era::modern &&
        (_method == "tasks/get" || _method == "tasks/cancel" ||
         _method == "tasks/update") &&
        supports_tasks()) {
        return detail::task_operation(_method, _params, _tasks);
    }
    if (_method == "prompts/list") {
        return on_prompts_list();
    }
    if (_method == "prompts/get") {
        const auto call = lock_inline_call();
        return on_prompts_get(_params);
    }
    if (_method == "resources/list") {
        return on_resources_list();
    }
    if (_method == "resources/read") {
        const auto call = lock_inline_call();
        return on_resources_read(_params);
    }
    return std::nullopt;
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

template <class Derived>
nlohmann::json Server<Derived>::on_initialize(const nlohmann::json &_params,
                                              const ServerInfo &_info) const {
    using nlohmann::json;
    const std::string requested = _params.value("protocolVersion", "");
    return json{
        {"protocolVersion", requested == kHandshakeVersion
                                ? requested
                                : std::string(kHandshakeVersion)},
        {"capabilities", capabilities()},
        {"serverInfo", _info.identity},
        {"instructions", _info.instructions},
    };
}

template <class Derived>
nlohmann::json Server<Derived>::on_discover(const ServerInfo &_info) const {
    using nlohmann::json;
    json caps = capabilities();
    if (supports_tasks()) {
        caps["extensions"][std::string(detail::kTasksExtension)] =
            json::object();
    }
    return json{
        {"resultType", "complete"},
        {"supportedVersions", json::array({kProtocolVersion})},
        {"capabilities", std::move(caps)},
        {"instructions", _info.instructions},
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

template <class Derived> bool Server<Derived>::supports_tasks() const {
    return std::ranges::any_of(m_tools, [](const Tool &_tool) {
        return _tool.run_coroutine || _tool.hints.background;
    });
}

template <class Derived>
nlohmann::json
Server<Derived>::on_tools_call(const nlohmann::json &_params, Era _era,
                               detail::TaskRuntime &_tasks) const {
    using nlohmann::json;
    const std::string name = _params.value("name", "");
    json args = _params.value("arguments", json::object());
    const Tool *tool = find_tool(name);
    if (tool == nullptr) {
        throw detail::RpcError(-32602, "Unknown tool: " + name);
    }
    if (!args.is_object()) {
        throw detail::RpcError(-32602, "Expected an arguments object");
    }
    if (tool->hints.background || tool->run_coroutine) {
        const bool return_handle =
            _era == Era::modern && detail::declares_tasks(_params);
        std::unique_lock<std::mutex> inline_call;
        if (!return_handle) {
            inline_call = lock_inline_call();
        }
        if (tool->run_coroutine) {
            auto work = detail::run_coroutine_task(tool->run_coroutine,
                                                   tool->self, std::move(args));
            return return_handle ? _tasks.submit(std::move(work))
                                 : _tasks.run(std::move(work));
        }
        auto work = [this, tool,
                     args = std::move(args)](detail::TaskContext _task) {
            const ActiveCall call(
                *this, detail::CallContext{.task = std::move(_task)});
            return run_tool(*tool, args);
        };
        return return_handle ? _tasks.submit(std::move(work))
                             : _tasks.run(std::move(work));
    }
    const auto call = lock_inline_call();
    return run_tool(*tool, args);
}

template <class Derived>
nlohmann::json Server<Derived>::run_tool(const Tool &_tool,
                                         const nlohmann::json &_args) {
    try {
        return _tool.run(_tool.self, _args);
    } catch (const std::exception &_e) {
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
            messages = prompt.run(prompt.self, args);
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
        json entry;
        try {
            entry = resource.run(resource.self, json::object());
        } catch (const std::exception &_e) {
            throw detail::RpcError(-32603, _e.what());
        }
        entry["uri"] = std::string(resource.uri);
        entry["mimeType"] = std::string(resource.mime_type);
        return json{
            {"resultType", "complete"},
            {"contents", json::array({std::move(entry)})},
            {"ttlMs", 300000},
            {"cacheScope", "public"},
        };
    }
    throw detail::RpcError(-32602, "Resource not found: " + uri);
}

}
