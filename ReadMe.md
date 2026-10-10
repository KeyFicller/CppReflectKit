# C++26 static reflection

Header-only libraries for WG21 [P2996](https://wg21.link/p2996) (`std::meta`),
built with the experimental
[clang-p2996](https://github.com/bloomberg/clang-p2996/tree/p2996).

Reflection here is the mechanism four layers are built on. Each reads a type's
shape at compile time and derives a surface from it, so the surface cannot drift
from the declaration:

| Library | Header | Derives |
|---|---|---|
| Undo | `lib/inc/undo/reflection_undo.h` | field capture / restore for `MEMBER()`-declared fields |
| UI | `lib/inc/ui/reflection_widgets.h` | one ImGui widget per reflected field type |
| JSON | `lib/inc/llm/reflection_json.h` | JSON Schema, OpenAI tool wrappers, and the reverse decoder |
| MCP | `lib/inc/llm/reflection_mcp.h` | tools / prompts / resources from marked member functions |

Each library has a design note beside it in `docs/`.

## Build

- Build a full `clang-p2996` install and set `LLVM_ROOT` to it: the prefix must
  contain `bin/clang++`, the reflective `include/c++/v1/meta` header and
  `lib/libc++.dylib`.
- Execute below command to generate executable.

```sh
cmake -S . -B build -G Ninja  #   Or "cmake.generator": "Ninja" in .vscode/settings.json
cmake --build build
```

## Libraries

### `undo::` — undo/redo (`reflection_undo.h`)

Declare each field with `MEMBER()`; the base reflects them, so capture and restore
need no per-type code.

**What you already have:** the whole object copied as the snapshot, or a
save/restore written per field:

```cpp
auto before = account;                 // one more copy of every field, every step
account.m_balance = 100;
account.m_log.push_back(3);
// undo: account = before;             // ...and the mirror to restore it
```

**What you add:** a base and a marker per field — one line per type the codec
knows:

```cpp
struct Account : undo::DbObject<Account> {
    explicit Account(undo::Session& _s) : undo::DbObject<Account>(_s) {}
    MEMBER(std::string, m_name);        // string
    MEMBER(int, m_balance, 0);          // scalar, starting at 0
    MEMBER(std::optional<int>, m_limit); // optional
    MEMBER(std::vector<int>, m_log);    // vector
};
```

**What you get:** each `MEMBER()` derives `touch_x()` / `get_x()`, and only the
fields actually written are recorded — undo restores exactly those:

```cpp
session.begin("Deposit");
account.open();
account.touch_m_balance() = 100;      // records the before value (0), once
account.touch_m_log() = { 1, 2, 3 };  // a vector, through the same call
account.close();
session.end();

account.get_m_balance();              // 100
session.undo();                       // both fields restored together
account.get_m_balance();              // 0
account.get_m_log();                  // {}
```

### `ui::` — widgets (`reflection_widgets.h`)

Walk a struct's members (and bases) by reflection and draw one ImGui widget per
field type; nested structs become collapsible groups, all in a two-column field
table.

**What you already have:** a hand-written ImGui call per member — the items list
for the enum, the presence box for the optional, the row loop for the vector, all
laid out and labelled by hand:

```cpp
ImGui::InputText("##name", &profile.m_name);
ImGui::Combo("##unit", &unit_index, "Celsius\0Fahrenheit\0");   // items list by hand
ImGui::Checkbox("##limit", &limit_present);                      // presence + value by hand
if (limit_present) ImGui::DragInt("##limit_v", &profile.m_limit.value());
for (std::size_t i = 0; i < profile.m_tags.size(); ++i) {        // row loop by hand
    ImGui::PushID(static_cast<int>(i)); ImGui::DragInt("##tag", &profile.m_tags[i]); ImGui::PopID();
}
ImGui::Checkbox("##active", &profile.m_active);
ImGui::TextUnformatted("Weight"); ImGui::SameLine();
ImGui::SliderInt("##weight", &profile.m_weight, 0, 100);
```

**What you add:** the struct — its field types pick the widgets, and one call
draws them:

```cpp
struct Profile {
    std::string m_name;                 // string   -> a text field
    Unit m_unit = Unit::Celsius;        // enum     -> a combo of the enumerators
    std::vector<int> m_tags;            // vector   -> rows with add / remove
    std::optional<int> m_limit;         // optional -> presence box + the inner widget
    bool m_active = false;              // bool     -> a checkbox

    [[= ui::label{ .text = ui::str("Weight") } ]]                    // rename this field
    [[= ui::control{ .fn = ^^ImGui::SliderInt, .v_min = 0.0, .v_max = 100.0 } ]]
    int m_weight = 50;                  // annotation -> a slider, not the default drag
};

ui::draw(profile);   // one widget per field, in declaration order
```

**What you get:** the whole form — labels in a fixed column, widgets in a
stretching one, one row per field, every control derived from its type:

```
m_name    [ Alice              ]
m_unit    [ Celsius         v  ]
m_tags    [x] 1
          [x] 2
          [+ Add]
m_limit   [x] [ 12 ]
m_active  [x]
Weight    [======50======]
```

### `js::` — JSON schema and decoding (`reflection_json.h`)

Compile-time projection of C++ types and functions to JSON, plus the reverse
decoder. No schema is hand-written.

**What you already have:** a struct, a function, and both schemas written out by
hand:

```cpp
struct Point { int x; int y; };
std::string greet(std::string name);

// ...the same two shapes again as JSON, kept in sync by hand
const char* kPointSchema = R"({"type":"object","properties":{ ... },"required":["x","y"]})";
const char* kGreetTool    = R"({"type":"function","function":{ ... }})";
```

**What you add:** a description on the declaration, once — on the type, its
fields, and a function's parameters:

```cpp
struct Point {
    [[= js::desc{ .text = js::str("Horizontal offset.") } ]] int x;
    [[= js::desc{ .text = js::str("Vertical offset.") } ]] int y;
};

[[= js::doc{ .text = js::str("Greets someone.") } ]]
[[= js::param_docs(js::str("Who to greet.")) ]]        // parameter descriptions, by position
std::string greet(std::string name);

constexpr auto object_schema = js::tool_schema<Point>();                            // object
constexpr auto tool          = js::function_schema<^^greet, js::Style::OpenAiTool>(); // tool
```

**What you get:** both shapes, generated at compile time:

```json
// js::tool_schema<Point>()
{ "type": "object",
  "properties": { "x": { "type": "integer", "description": "Horizontal offset." },
                  "y": { "type": "integer", "description": "Vertical offset." } },
  "required": ["x", "y"] }

// js::function_schema<^^greet, js::Style::OpenAiTool>()
{ "type": "function",
  "function": { "name": "greet",
                "description": "Greets someone.",
                "parameters": { "type": "object",
                                "properties": { "name": { "type": "string",
                                                          "description": "Who to greet." } },
                                "required": ["name"] } } }
```

### `mcp::` — MCP server (`reflection_mcp.h`)

A CRTP base whose constructor *scans* the derived class: a marked **non-static
member function** becomes a tool, prompt or resource, with no registration line.
It owns the JSON-RPC 2.0 loop and the three tables, and speaks protocol revision
`mcp::kProtocolVersion` (`"2026-07-28"`).

**What you already have:** the functions, and a hand-written table entry,
descriptor and unpacking lambda per tool, prompt and resource:

```cpp
std::string greet(std::string name);
std::string explain(std::string topic);

server.add_tool("greet", R"({"description":"Greets someone.","inputSchema":{ ... }})",
                [](const json& a) { return greet(a["name"]); });
server.add_prompt("explain", R"({"arguments":[ ... ]})", /* ... */);
server.add_resource("file:///docs/notes.md", "text/markdown", /* ... */);
```

**What you add:** a marker and its doc on each function:

```cpp
class MyServer : public mcp::Server<MyServer> {
    [[= mcp::tool{}]]
    [[= js::doc{ .text = js::str("Greets someone.") } ]]
    [[= js::param_docs(js::str("Who to greet.")) ]]
    std::string greet(std::string name);

    [[= mcp::prompt{}]]
    [[= js::doc{ .text = js::str("Explain a concept.") } ]]
    [[= js::param_docs(js::str("The concept to explain.")) ]]
    std::string explain(std::string topic);

    [[= mcp::resource{ .uri = js::str("file:///docs/notes.md"),
                       .mime_type = js::str("text/markdown") }]]
    [[= js::doc{ .text = js::str("Design notes.") } ]]
    std::string design_notes();
};
```

**What you get:** `tools/list`, `prompts/list` and `resources/list` serve these —
name, schema, `required` and the call all derived from the signatures:

```json
// tools/list
{ "name": "greet", "description": "Greets someone.",
  "inputSchema": { "type": "object",
                   "properties": { "name": { "type": "string",
                                             "description": "Who to greet." } },
                   "required": ["name"] } }

// prompts/list
{ "name": "explain", "description": "Explain a concept.",
  "arguments": [ { "name": "topic", "description": "The concept to explain.",
                   "required": true } ] }

// resources/list
{ "uri": "file:///docs/notes.md", "name": "design_notes",
  "description": "Design notes.", "mimeType": "text/markdown" }
```

## Design notes

- [`docs/about_llm.md`](docs/about_llm.md) — JSON schema, MCP tools/prompts/resources, and the LLM bridge.
- [`docs/about_ui.md`](docs/about_ui.md) — the widget walk, control annotations and overrides.
- [`docs/about_undo.md`](docs/about_undo.md) — the member/index codec, commands and transactions.
