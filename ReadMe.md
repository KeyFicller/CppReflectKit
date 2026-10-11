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
table. Where the type's default is not the one you want, an annotation on the
field names the ImGui function instead — the widget is data, not code.

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

**What you add:** the struct — its field types pick the widgets, one call draws
them, and a `ui::control` on a field swaps in a different ImGui function, its
parameters named as it declares them:

```cpp
struct Profile {
    std::string m_name;                 // string   -> a text field
    Unit m_unit = Unit::Celsius;        // enum     -> a combo of the enumerators
    std::vector<int> m_tags;            // vector   -> rows with add / remove
    std::optional<int> m_limit;         // optional -> presence box + the inner widget
    bool m_active = false;              // bool     -> a checkbox

    [[= ui::label{ .text = ui::str("Weight") } ]]                    // rename this field
    [[= ui::control{ .fn = ^^ImGui::SliderInt, .v_min = 0.0, .v_max = 100.0 }]]
        int m_weight = 50;              // a drag by default; this is a slider

    [[= ui::control{ .fn = ^^ImGui::InputInt, .step = 1.0, .step_fast = 10.0 }]]
        int m_retries = 3;              // a third function, its own parameters
};

ui::draw(profile);   // one widget per field, in declaration order
```

**What you get:** the whole form — labels in a fixed column, widgets in a
stretching one, one row per field, every control derived from its type or its
annotation:

```
m_name    [ Alice              ]
m_unit    [ Celsius         v  ]
m_tags    [x] 1
          [x] 2
          [+ Add]
m_limit   [x] [ 12 ]
m_active  [x]
Weight    [======50======]
m_retries [ 3 ] [+10]
```

One annotation, `ui::control`: `fn` is the function, the rest are that function's
parameters by name, so `SliderInt` takes `v_min`/`v_max` and `InputInt` takes
`step`/`step_fast` — a member `fn` does not take is ignored. It is checked at
compile time: an `fn` outside the vocabulary, or one whose value parameter does
not match the field's type, is a `static_assert`.

To change the widget for a whole **type** rather than one field, that is one
specialization instead of an annotation on every use — `default_control<T>` for a
type the vocabulary already covers, `Control<T>` for one of your own:

```cpp
struct Vec2 { float x; float y; };
namespace ui {
template <> struct default_control<int> {           // every int in the form
    static constexpr control value{ .fn = ^^ImGui::SliderInt, .v_min = 0, .v_max = 100 };
};
template <> struct Control<Vec2> {                   // a leaf, not a nested group
    void draw(Vec2& v) { ImGui::DragFloat2("##vec", &v.x); }
};
}
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

## Build

- Build a full `clang-p2996` install and point the build at it — the prefix must
  contain `bin/clang++`, the reflective `include/c++/v1/meta` header and
  `lib/libc++.dylib`. It is found in this order: `-DCPPREF_LLVM_ROOT=`, the
  `CPPREF_LLVM_ROOT` environment variable, the prefix of the compiler in use, a
  `clang++` on `PATH`. To pin a machine-specific prefix, copy
  `CMakeUserPresets.json.example` to the gitignored `CMakeUserPresets.json` and
  fill in `CPPREF_LLVM_ROOT`, then use `cmake --preset local`.
- Execute below command to generate executable.

```sh
cmake --preset local          # or "default", when CPPREF_LLVM_ROOT is already exported
cmake --build --preset local
```

## Consume from another project

The four libraries are `INTERFACE` targets, so an external project links one name
and its headers, dependencies and compiler flags come with it:

```cmake
include(FetchContent)
FetchContent_Declare(cppreflect
  GIT_REPOSITORY  <this repository>
  GIT_TAG         <commit>)
FetchContent_MakeAvailable(cppreflect)

target_link_libraries(app PRIVATE cppreflect::json)   # also brings nlohmann/json
```

| Target | Needs |
|---|---|
| `cppreflect::undo` | — |
| `cppreflect::ui` | Dear ImGui |
| `cppreflect::json` | nlohmann/json |
| `cppreflect::mcp` | `cppreflect::json` |

Each target carries `lib/inc`, `-freflection-latest`, C++26 and the compiler's
SDK / libc++ paths, so nothing is configured twice; the consumer only has to
build with `clang-p2996` itself. `nlohmann/json` and Dear ImGui are taken from
`find_package` first and fetched only when missing, and an import builds none of
this project's tests. A slimmer import turns the optional halves off:

```sh
cmake -DCPPREF_WITH_UI=OFF -DCPPREF_WITH_LLM=OFF ...   # then only cppreflect::undo exists
```

`add_subdirectory(path/to/CppReflectKit)` works the same way.

## Design notes

- [`docs/about_undo.md`](docs/about_undo.md) — the member/index codec, commands and transactions.
- [`docs/about_ui.md`](docs/about_ui.md) — the widget walk, control annotations and overrides.
- [`docs/about_llm.md`](docs/about_llm.md) — JSON schema, MCP tools/prompts/resources, and the LLM bridge.
- [`docs/about_dispatch.md`](docs/about_dispatch.md) — calling a function by parameter name, for tool calls and widget controls alike.

## License

[MIT](LICENSE).
