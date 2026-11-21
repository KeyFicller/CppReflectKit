# Exposing existing C++ to an LLM

Add two attribute lines to a function you already have. Reflection derives the JSON
Schema, the OpenAI tool wrapper, the MCP descriptors and the typed call — nothing is
hand-written, and nothing can drift from the signature.
Build prerequisites: [`../ReadMe.md`](../ReadMe.md).

## The deal

```cpp
// What you already have:
std::string search_documents(std::string query, int limit, bool exact_match);
```

```cpp
// What you add — a marker and optional docs:
[[= mcp::tool{}]]
[[= js::doc{ .text = js::str("Searches the repository's own documentation.") } ]]
[[= js::param_docs(js::str("The raw query text."), js::str("Maximum number of hits to return."),
                   js::str("Require an exact (case-sensitive) match.")) ]]
std::string search_documents(std::string query, int limit, bool exact_match);
```

From those lines, at compile time:

```json
{ "name": "search_documents", "description": "Searches the repository's own documentation.",
  "inputSchema": {
    "type": "object",
    "properties": {
      "query":       { "type": "string",  "description": "The raw query text." },
      "limit":       { "type": "integer", "description": "Maximum number of hits to return." },
      "exact_match": { "type": "boolean", "description": "Require an exact (case-sensitive) match." } },
    "required": ["query", "limit", "exact_match"],
    "additionalProperties": false } }
```

Name, enum and `required` all come from the signature: every parameter is required
because none is `std::optional`. Change the C++ and the schema follows.

### Doxygen comments and reflected descriptions

Doxygen renders the comment blocks. MCP and JSON Schema read the C++ annotations.

- A function or type's `@brief` corresponds to its `js::doc` text.
- A data member's `@brief` corresponds to its `js::desc` text.
- Doxygen `@param` names each parameter. `js::param_docs` lists the descriptions in declaration order.
- Use `@details` and `@return` for additional API documentation. [Doxygen commands](https://www.doxygen.nl/manual/commands.html)

```cpp
/**
 * @brief Searches the repository's own documentation.
 * @param query The raw query text.
 * @param limit Maximum number of hits to return.
 * @param exact_match Require an exact (case-sensitive) match.
 * @return The matches as JSON text.
 */
[[= mcp::tool{}]]
[[= js::doc{.text = js::str("Searches the repository's own documentation.")}]]
[[= js::param_docs(js::str("The raw query text."),
                   js::str("Maximum number of hits to return."),
                   js::str("Require an exact (case-sensitive) match."))]]
std::string search_documents(std::string query, int limit, bool exact_match);

struct SearchOptions {
    /**
     * @brief Maximum number of hits to return.
     */
    [[= js::desc{.text = js::str("Maximum number of hits to return.")}]]
    int limit = 5;
};
```

## How the derivation works

**1. The marker is the registration.** The base is CRTP; its constructor scans the
derived class, so there is no registration call, no static initializer, no macro:

```cpp
template <std::meta::info M, typename Self>
void Server<Derived>::register_member(Self* _self)
{
    // Constructors, the destructor and operators have no identifier; data members
    // and nested types are not functions. Only marked functions register.
    if constexpr (!std::meta::is_function(M) || !std::meta::has_identifier(M)) {
        return;
    } else if constexpr (detail::has_annotation<^^mcp::tool, M>()) {
        m_tools.push_back(Tool{std::meta::identifier_of(M), detail::tool_description<M>(),
                               js::function_schema<M, js::Style::JsonSchema>(), _self,
                               &detail::invoke_tool_entry<M, Self>,
                               detail::coroutine_tool<M, Self>(), detail::tool_marker<M>()});
    }
    // … prompt / resource identical, different marker
}
```

The scan expands the static reflection range with `template for`:

```cpp
template <class Derived>
void Server<Derived>::register_members()
{
    auto* self = static_cast<Derived*>(this);
    constexpr auto members = std::define_static_array(
        std::meta::members_of(^^Derived, std::meta::access_context::unchecked()));
    template for (constexpr auto member : members) {
        register_member<member>(self);
    }
}
```

**2. One parameter model, `consteval`.** Everything about a parameter is read once,
off the same `std::meta::parameters_of(Fn)`:

```cpp
// cppreflect::detail
template <std::meta::info Fn> consteval std::size_t param_count();
template <std::meta::info Fn, std::size_t I> consteval std::meta::info param_at();
// js::detail
template <std::meta::info Fn, std::size_t I> consteval std::string param_description();
```

Assembled into the `inputSchema` — `param_schema<PT>()` maps the type (`int` →
`"type":"integer"`, `Unit` → `"enum":[…]`, `std::optional<Unit>` → `anyOf[…]`, a
reflected struct → a nested object):

```cpp
properties += "\"" + name + "\":" + with_description(param_schema<PT>(), param_description<Fn, Is>());
required += "\"" + name + "\"";   // std::optional included; its schema is anyOf [T, null]
```

**3. Two emitters, one table.** A row stores the *bare* `inputSchema` plus name and
description; `--emit-tools` wraps it for OpenAI, MCP `tools/list` serves it flat — so
the OpenAI client and the MCP client **cannot disagree**:

```cpp
enum class Style { JsonSchema, OpenAiTool };                  // same params, two wrappers
consteval std::string_view function_schema();                 // the tool's parameters
// js::detail::build_openai_tool also wraps the stored schema at runtime.
```

**4. The call is derived too.** The model's `arguments` decode into a real call with
the same rules that produced `required`:

```cpp
template <std::meta::info Fn, typename Self, std::size_t... Is>
auto invoke_member_impl(Self& _self, const nlohmann::json& _j, std::index_sequence<Is...>)
{
    std::tuple<param_type_at<Fn, Is>...> args{};
    (assign_argument<Fn, Is>(args, _j), ...);          // throws on a missing required arg
    return (_self.*&[: Fn :])(std::get<Is>(args)...);  // parameters_of excludes `this`
}
```

One reflection pass feeds the schema; the same reflections drive the call, so a
hand-edited schema is impossible.

## What is derived, per surface

| From one `^^fn` | Projection |
|---|---|
| `identifier_of` | tool / prompt / resource **name** |
| `js::doc` | **description**, all three — keyed on the entity, not the role |
| `param_count` / `param_at` / `param_description` | tool `inputSchema`; prompt `arguments`; resource template vars |
| `invoke_member_with_json<Fn>` | the call (tool and prompt) |

MCP uses JSON Schema only for tools; prompts use
`arguments: [{name, description?, required?}]` and resource templates use RFC 6570.
That is a difference in **wire shape**, not in **authoring** — a prompt argument is
`identifier_of` + `param_description` + `!is_optional` over the same parameter
reflections, and a prompt returns the messages payload instead of a result:

```json
{ "name": "explain", "arguments": [ { "name": "topic", "required": true } ] }
```

**You write:** the marker, optionally the docs, and the body. A resource also
carries its URI and MIME in the marker (the one template marker) because reflection
cannot invent a `file:///…` URI. Limits: prompt arguments are always strings (a
string-like `static_assert`), and resource templates carry no argument descriptors.

## Consuming it

`reflection_json.h` exposes the annotations, schema generators, JSON codecs,
and invocation functions. Use `js::Style::OpenAiTool` or
`Server::tools_as_openai_json()` to generate OpenAI tool wrappers.
`CPP_REFLECT_MODEL` registers a model. `js::model_registry()` provides read-only
access to those registrations.

`reflection_mcp.h` exposes `mcp::Server`, the member annotations and result types,
and the coroutine return type. Keep resource URIs in the annotation or a shared
constant when the member body also needs the URI. The `detail` namespaces contain
implementation helpers.

One dispatch — `bridge::run_request` (`test/llm/scaffold/bridge.cpp`) — serves the argv path and the socket
server, so a debugged session runs exactly what a plain invocation runs.

| Command | Purpose |
|---|---|
| `--emit-tools` / `--call <tool>` | OpenAI schemas for the model / run one tool from JSON on stdin |
| `--emit-schemas` / `--parse <Model>` | struct schemas / reconstruct a struct |
| `--mcp` | the same table over MCP (`tools` / `prompts` / `resources`) |
| `--serve [sock]` | long-running debug server (`/tmp/cpp_reflect.sock`) |

Errors go to **stderr**, exit 1; `--mcp` routes before the generic `--*` branch
(`test/llm/scaffold/main.cpp`) or it throws `unknown command: --mcp`.

```sh
python test/llm/scaffold/llm_client.py tools      [question]           # --emit-tools + --call
python test/llm/scaffold/llm_client.py mcp        [--prompt NAME [k=v ...]] [--resource URI] [question]
python test/llm/scaffold/llm_client.py structured [request] [format]   # strict positional; both or neither
```

`llm_client.py` never redeclares a C++ signature. `mcp` mode goes through
LangChain's `MCPAdapter` (FastMCP beneath); only tools reach the model — prompts are
the *user's* pick (`prompts/get` messages open the chat), resources the
*application's* context, each by the actor MCP assigns it. `requirements.txt` →
`.venv`; `.env` needs `DEEPSEEK_API_KEY`. (`make_model()` works around the API
rejecting `response_format: json_schema` by sending a tool schema — which forces
`tool_choice`, refused by thinking mode unless `reasoning_effort="none"`.)

## Long-running MCP tools

Declare background execution at the tool definition. A synchronous tool sets
`background`. A coroutine tool returns `mcp::awaitable<T>`. Both start on the
same worker pool, which runs the standalone Asio event loop. The tool can use
operations with compatible C++ awaiters. The `cppreflect::mcp` target uses
installed Asio headers or fetches a pinned release when `CPPREF_FETCH_DEPS` is
enabled.

```cpp
[[= mcp::tool{.background = true}]]
std::string calculate(int iterations);

[[= mcp::tool{}]] mcp::awaitable<std::string> load_report(std::string key) {
    co_return co_await reports.load(std::move(key));
}
```

Here `reports.load()` is an awaitable operation supplied by the application.
When an operation suspends, the worker can start another task. The awaiter
chooses when and on which thread the coroutine resumes. A blocking call uses
a worker for its duration. Protect shared state across calls and complete
without elicitation.

The pool defaults to two workers. A synchronous tool can check
`task_stop_requested()` between chunks of work. A coroutine can retrieve a
`std::stop_token` with `co_await mcp::this_coro::stop_token` and pass it to an
operation that supports cancellation.

A `2026-07-28` client declares `io.modelcontextprotocol/tasks` in the
`extensions` object under `params._meta["io.modelcontextprotocol/clientCapabilities"]`.
The `tools/call` response then carries a `taskId`. Poll `tasks/get` for the
result or send `tasks/cancel`. Declare the extension on each task request. On
HTTP, set `Mcp-Name` to the task ID on these requests. Legacy clients wait for
the tool and receive its result on `tools/call`.

For a tool that returns a task handle, `tasks/get` exposes its latest progress
as `statusMessage` and updates `lastUpdatedAt` when the message changes. A
synchronous background tool calls `report_task_progress("Indexed 42 / 100 files")`.
A coroutine tool uses `co_await mcp::this_coro::report_progress("Indexed 42 / 100 files")`
to report without suspending.

Messages must be valid UTF-8 and at most 1024 bytes. Report an empty string to
clear the message. The task clears progress when it finishes or cancellation
is requested. Direct calls that return the tool result ignore progress reports.

### Deploying HTTP

Pass a host to `serve_http(host, port)` to select the listening address.
`serve_http(port)` listens on 127.0.0.1. The example CLI accepts
`--http 8765 0.0.0.0` to listen on all IPv4 interfaces.

Place the listener behind a reverse proxy that handles TLS, client
authentication, and rate limiting. Route requests for a task to the process
that created it. Treat task IDs as credentials when configuring proxy logs.
Task records are held in memory and expire one minute after the task ends.

Each serving loop accepts up to 32 outstanding jobs. It holds up to
1,024 task records, with a 128 KiB limit on each recorded result. The HTTP body
limit is 256 KiB. When task admission is saturated, it can wait up to 500 ms for
capacity. Task admission overload returns HTTP `503` with `Retry-After`. The
HTTP socket queue may close excess connections. HTTP runs eight request
workers. Up to four synchronous tool calls, prompt renders, and resource reads
can run at once so control requests have room to proceed. If all four slots
are occupied, the server returns HTTP `503` with `Retry-After` for further
synchronous calls. Protect shared mutable state in application callbacks.

Call `stop_http()` and wait for `serve_http()` to return before destroying the
server object. Shutdown requests cancellation and waits for accepted tools
to finish, including suspended coroutines. Awaiters used in long-running tools
should arrange completion or respond to the stop token. The stdio serving
loop starts shutdown when it reaches EOF. A synchronous stdio call waits for
the tool to finish before the loop can read the next request or observe EOF.
