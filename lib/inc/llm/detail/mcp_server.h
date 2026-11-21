#pragma once

#include "mcp_tasks.h"
#include "mcp_tools.h"

#include <atomic>
#include <cstddef>
#include <httplib.h>
#include <iosfwd>
#include <meta>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcp {

/**
 * @brief The protocol revision this server speaks.
 */
inline constexpr std::string_view kProtocolVersion = "2026-07-28";

/**
 * @brief Supported revision for clients that use the initialize handshake.
 */
inline constexpr std::string_view kHandshakeVersion = "2025-11-25";

namespace detail {

struct RpcReply;

struct CallContext {
    const nlohmann::json *params = nullptr;
    TaskContext task{};
    bool asked = false;
};

}

/**
 * @brief Serves the annotated tools, prompts and resources of a derived class.
 * @tparam Derived The application server type.
 * @details HTTP requests can invoke application callbacks concurrently.
 * Synchronize shared mutable state accessed by these callbacks.
 */
template <class Derived> class Server {
public:
    /**
     * @brief Copy construction is disabled for server instances.
     */
    Server(const Server &) = delete;
    /**
     * @brief Copy assignment is disabled for server instances.
     */
    Server &operator=(const Server &) = delete;
    /**
     * @brief Releases the server and its HTTP listener.
     * @note Wait for serve_http() to return before destroying the server.
     */
    virtual ~Server();

    /**
     * @brief Serves MCP over newline-delimited JSON-RPC streams.
     * @param _in Stream containing JSON-RPC requests.
     * @param _out Stream receiving protocol replies.
     * @return 0 at end of input.
     */
    int serve(std::istream &_in, std::ostream &_out);

    /**
     * @brief Serves MCP on standard input and output.
     */
    int stdio();

    /**
     * @brief Serves /mcp on 127.0.0.1 until stopped.
     * @param _port Listening port. Pass 0 to select an available port.
     * @return 0 when stopped, or 1 on bind failure.
     * @note Wait for this call to return before destroying the server.
     */
    int serve_http(int _port) { return serve_http("127.0.0.1", _port); }

    /**
     * @brief Serves /mcp on the specified address until stopped.
     * @param _host Listening hostname or IP address. Use an unbracketed
     * IPv6 address, or 0.0.0.0 to listen on all IPv4 interfaces.
     * @param _port Listening port. Pass 0 to select an available port.
     * @return 0 when stopped, or 1 on bind failure.
     * @note Wait for this call to return before destroying the server.
     */
    int serve_http(std::string_view _host, int _port);

    /**
     * @brief Stops the HTTP listener.
     * @note May be called from another thread.
     */
    void stop_http();

    /**
     * @brief The port serve_http() bound, or -1 when it is not listening.
     */
    int http_port() const { return m_http_port.load(); }

    /**
     * @brief Returns the registered tools as an OpenAI JSON array.
     */
    std::string tools_as_openai_json() const;

    /**
     * @brief Calls a registered tool.
     * @param _name The registered tool name.
     * @param _args The tool arguments as a JSON object.
     * @return The text output of the tool.
     * @throws std::runtime_error If the tool is unknown or fails.
     */
    std::string call_tool(std::string_view _name,
                          const nlohmann::json &_args) const;

protected:
    /**
     * @brief Registers the annotated members of Derived.
     * @param _workers Number of task workers.
     * @throws std::invalid_argument If _workers is zero.
     */
    explicit Server(std::size_t _workers = 2) : m_workers(_workers) {
        if (_workers == 0) {
            throw std::invalid_argument("MCP needs at least one task worker");
        }
        register_members();
    }

    /**
     * @brief Supplies the server name reported to MCP clients.
     * @return The name, backed by storage valid until the server copies it.
     */
    virtual std::string_view server_name() const = 0;
    /**
     * @brief Supplies the server version reported to MCP clients.
     * @return The version, backed by storage valid until the server copies it.
     */
    virtual std::string_view server_version() const = 0;
    /**
     * @brief Supplies optional instructions reported to MCP clients.
     * @return The instructions, or an empty view to omit them.
     */
    virtual std::string_view instructions() const { return {}; }

    /**
     * @brief Requests one flat form per call.
     * @details A client retry restarts the member. Ask before side effects and
     * let the suspension signal propagate.
     * @tparam T The reflected type defining the form fields.
     * @param _message The prompt shown to the user.
     * @return The elicitation outcome and the accepted value, if available.
     */
    template <typename T>
    elicit_result<T> elicit_form(std::string _message) const;

    /**
     * @brief Poll from a running synchronous task to stop cooperatively after
     * cancellation.
     */
    bool task_stop_requested() const;

    /**
     * @brief Reports a polling status message from a synchronous task.
     * @param _message UTF-8 status text of at most 1024 bytes. An empty message
     * clears the current text.
     * @details Invalid messages are ignored. Progress is exposed for calls
     * that return a task handle.
     */
    void report_task_progress(std::string_view _message) const;

private:
    enum class Era { modern, handshake };

    struct Tool {
        std::string_view name;
        std::string_view description;
        std::string_view input_schema;
        void *self;
        nlohmann::json (*run)(void *, const nlohmann::json &);
        detail::CoroutineTool run_coroutine;
        tool hints;
    };
    struct Prompt {
        std::string_view name;
        std::string_view description;
        std::string_view arguments;
        void *self;
        nlohmann::json (*run)(void *, const nlohmann::json &);
    };
    struct Resource {
        std::string_view uri;
        std::string_view mime_type;
        std::string_view name;
        std::string_view description;
        void *self;
        nlohmann::json (*run)(void *, const nlohmann::json &);
    };

    struct HttpRun {
        explicit HttpRun(std::size_t _workers) : tasks(_workers) {}
        detail::TaskRuntime tasks;
        httplib::Server http;
        std::atomic<bool> stopping{false};
    };

    struct ServerInfo {
        nlohmann::json identity;
        std::string instructions;
    };

    template <std::meta::info M, typename Self>
    void register_member(Self *_self);

    void register_members();

    class ActiveCall {
    public:
        ActiveCall(const Server &_server, detail::CallContext _context)
            : m_server(_server), m_context(_context) {
            std::lock_guard lock(m_server.m_calls_mutex);
            auto &active = m_server.m_calls[std::this_thread::get_id()];
            m_previous = std::exchange(active, &m_context);
        }

        ActiveCall(const ActiveCall &) = delete;
        ActiveCall &operator=(const ActiveCall &) = delete;

        ~ActiveCall() {
            std::lock_guard lock(m_server.m_calls_mutex);
            const auto it = m_server.m_calls.find(std::this_thread::get_id());
            if (m_previous) {
                it->second = m_previous;
            } else {
                m_server.m_calls.erase(it);
            }
        }

    private:
        const Server &m_server;
        detail::CallContext m_context;
        detail::CallContext *m_previous;
    };

    static constexpr std::ptrdiff_t kMaxSynchronousCalls = 4;

    class SynchronousCall {
    public:
        explicit SynchronousCall(const Server &_server) : m_server(_server) {
            if (!m_server.m_synchronous_slots.try_acquire()) {
                throw detail::TaskUnavailable("MCP synchronous calls are busy");
            }
        }

        SynchronousCall(const SynchronousCall &) = delete;
        SynchronousCall &operator=(const SynchronousCall &) = delete;
        ~SynchronousCall() { m_server.m_synchronous_slots.release(); }

    private:
        const Server &m_server;
    };

    detail::CallContext *current_call() const;
    ServerInfo snapshot_server_info() const;
    detail::RpcReply dispatch(const nlohmann::json &_request, Era &_era,
                              detail::TaskRuntime &_tasks,
                              const ServerInfo &_info);
    std::optional<nlohmann::json> handle_method(std::string_view _method,
                                                const nlohmann::json &_params,
                                                Era _era,
                                                detail::TaskRuntime &_tasks,
                                                const ServerInfo &_info) const;
    const Tool *find_tool(std::string_view _name) const;
    bool supports_tasks() const;
    nlohmann::json capabilities() const;
    nlohmann::json on_initialize(const nlohmann::json &_params,
                                 const ServerInfo &_info) const;
    nlohmann::json on_discover(const ServerInfo &_info) const;
    nlohmann::json on_tools_list() const;
    nlohmann::json on_tools_call(const nlohmann::json &_params, Era _era,
                                 detail::TaskRuntime &_tasks) const;
    static nlohmann::json run_tool(const Tool &_tool,
                                   const nlohmann::json &_args);
    nlohmann::json on_prompts_list() const;
    nlohmann::json on_prompts_get(const nlohmann::json &_params) const;
    nlohmann::json on_resources_list() const;
    nlohmann::json on_resources_read(const nlohmann::json &_params) const;

    std::vector<Tool> m_tools;
    std::vector<Prompt> m_prompts;
    std::vector<Resource> m_resources;

    const std::size_t m_workers;
    mutable std::counting_semaphore<kMaxSynchronousCalls> m_synchronous_slots{
        kMaxSynchronousCalls};
    mutable std::mutex m_calls_mutex;
    mutable std::unordered_map<std::thread::id, detail::CallContext *> m_calls;

    std::mutex m_http_mutex;
    HttpRun *m_http = nullptr;
    std::atomic<int> m_http_port{-1};
};

template <class Derived>
template <std::meta::info M, typename Self>
void Server<Derived>::register_member(Self *_self) {
    if constexpr (!std::meta::is_function(M) || !std::meta::has_identifier(M)) {
        return;
    } else if constexpr (detail::has_annotation<^^mcp::tool, M>()) {
        m_tools.push_back(
            Tool{std::meta::identifier_of(M), detail::tool_description<M>(),
                 js::function_schema<M, js::Style::JsonSchema>(), _self,
                 &detail::invoke_tool_entry<M, Self>,
                 detail::coroutine_tool<M, Self>(), detail::tool_marker<M>()});
    } else if constexpr (detail::has_annotation<^^mcp::prompt, M>()) {
        m_prompts.push_back(Prompt{std::meta::identifier_of(M),
                                   detail::tool_description<M>(),
                                   detail::prompt_arguments<M>(), _self,
                                   &detail::invoke_prompt_entry<M, Self>});
    } else if constexpr (detail::has_annotation<^^mcp::resource, M>()) {
        constexpr auto resource = detail::resource_marker<M>();
        m_resources.push_back(
            Resource{detail::static_text(resource.uri.data),
                     detail::static_text(resource.mime_type.data),
                     std::meta::identifier_of(M), detail::tool_description<M>(),
                     _self, &detail::invoke_resource_entry<M, Self>});
    }
}

template <class Derived> void Server<Derived>::register_members() {
    auto *self = static_cast<Derived *>(this);
    constexpr auto members = std::define_static_array(std::meta::members_of(
        ^^Derived, std::meta::access_context::unchecked()));
    template for (constexpr auto member : members) {
        register_member<member>(self);
    }
}

}
