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
    } else if constexpr (has_annotation<M, ^^mcp::tool>()) {
        m_tools.push_back(Tool{std::meta::identifier_of(M), tool_description<M>(),
                               js::function_schema<M, js::Style::JsonSchema>(), _self,
                               &detail::invoke_tool_entry<M, Self>});
    }
    // … prompt / resource identical, different marker
}
```

The scan unrolls over `std::index_sequence` (on this fork `template for` over a
reflection range fails):

```cpp
template <std::meta::info Class, typename Self, std::size_t... Is>
void Server<Derived>::scan(Self* _self, std::index_sequence<Is...>)
{
    constexpr auto infos = class_member_infos<Class>();
    (register_member<infos[Is]>(_self), ...);
}
```

**2. One parameter model, `consteval`.** Everything about a parameter is read once,
off the same `std::meta::parameters_of(Fn)`:

```cpp
template <meta::info Fn> consteval std::size_t param_count();
template <meta::info Fn, std::size_t I> consteval meta::info param_at();
template <meta::info Fn, std::size_t I> consteval std::string param_description();
```

Assembled into the `inputSchema` — `param_schema<PT>()` maps the type (`int` →
`"type":"integer"`, `Unit` → `"enum":[…]`, `std::optional<Unit>` → `anyOf[…]`, a
reflected struct → a nested object):

```cpp
properties += "\"" + name + "\":" + with_description(param_schema<PT>(), param_description<Fn, Is>());
if constexpr (!is_optional<PT>::value && !meta::has_default_argument(param_at<Fn, Is>())) {
    required += "\"" + name + "\"";
}
```

**3. Two emitters, one table.** A row stores the *bare* `inputSchema` plus name and
description; `--emit-tools` wraps it for OpenAI, MCP `tools/list` serves it flat — so
the OpenAI client and the MCP client **cannot disagree**:

```cpp
enum class Style { JsonSchema, OpenAiTool };                  // same params, two wrappers
consteval std::string_view function_schema();                 // the tool's parameters
constexpr std::string build_openai_tool(name, desc, params);  // {"type":"function", …}
```

**4. The call is derived too.** The model's `arguments` decode into a real call with
the same rules that produced `required`:

```cpp
template <meta::info Fn, typename Self, std::size_t... Is>
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
