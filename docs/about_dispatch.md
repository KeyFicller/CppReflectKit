# Calling a function by parameter name

A call arrives with its arguments **named, not positioned**. The names are the
target function's parameter names, and the dispatcher matches them up itself. One
idea: **a parameter's name is its identity, and every surface — the generated
schema, the annotation, the call — is read from one `parameters_of(Fn)`, so they
cannot disagree.** Build prerequisites: [`../ReadMe.md`](../ReadMe.md).

The binding model is Python's **keyword arguments** — a name selects the parameter —
carried across a data boundary: the signature is fixed at compile time, the
arguments arrive as data.

Two callers in this project work this way, and they share the whole mechanism:

| | A tool call | A widget control |
|---|---|---|
| The function | a C++ member (the tool) | an ImGui function, named by `fn` |
| Names the parameters | the model's JSON object | a `ui::control` annotation |
| Values resolved | at run time | at compile time |
| Kept honest by | the schema, from the same reflection | two `static_assert`s |
| Entry point | `js::invoke_member_with_json` | `ui::draw_control` |

## The deal

```cpp
// A tool: the object is keyed by the C++ parameter names.
std::string search_documents(std::string query, int limit, bool exact_match);
```

```json
{ "query": "meta", "limit": 10, "exact_match": true }
```

```cpp
// A control: the annotation names the function and carries its parameters as
// members of those names.
[[= ui::control{.fn = ^^ImGui::SliderInt, .v_min = -10, .v_max = 10}]] int m_volts;
```

## How the dispatch works

Both callers run the same four steps over `parameters_of(Fn)`. The source comments
say so outright — the widget layer's introspection is written "as in the JSON
layer's tool call".

**1. The parameters are reflected, once.** Three `consteval` helpers, identical in
both layers, turn the signature into compile-time facts:

```cpp
consteval std::size_t param_count();                    // parameters_of(Fn).size()
consteval std::meta::info param_at<Fn, I>();            // the I'th parameter
using param_type_at = …;                                // its bare type
```

**2. One value-initialized slot per parameter.** Counted and typed from the
signature, so the tuple's shape *is* the parameter list:

```cpp
std::tuple<param_type_at<Fn, Is>...> args{};
```

**3. Each slot is filled by name.** This is the only step where the two callers
differ, and it is where the name becomes a value:

```cpp
// tool call -- the slot by index, the value by the document's key
std::get<I>(_args) = from_json<PT>(_j.at(key));

// control -- the parameter's name matched against the annotation's members
constexpr std::meta::info member = control_argument_of<^^control, param_at<Fn, I>()>();
```

**4. The tuple is expanded into the real call.** A runtime tuple cannot be splatted
into a fixed parameter list, so the indices that filled it expand it — the same
`std::apply` in both layers, or a pointer-to-member for a tool:

```cpp
std::apply(&[: Fn :], args);                        // free function, and a control
(_self.*&[: Fn :])(std::get<Is>(args)...);          // member function — what a tool uses
```

A pointer-to-member cannot be formed from a runtime reflection, so a tool's
registry entry stores a compile-time trampoline instead. `parameters_of` excludes
the implicit object parameter, so the indices still line up with the schema.

## Step 3, in each direction

**A tool call looks values up in a document.** The slot is chosen by position, the
value by name, so key order is irrelevant. A key matching no parameter is a
`bind_error`. Each level of decoding prefixes the message with the name it was
working on,
so an error points at the offending argument rather than at the call.

**A control reads values out of an annotation.** For parameter `I` of the named
function, `control_argument_of` looks for a `control` member with that identifier
and uses it if found. The rest are filled from context rather than the annotation:
`label` from the walk, and the parameters that describe the value — `v`, `p_data`,
`str`, `buf`, a `buf_size` taken from a char array's extent, or a `data_type` from
the value's type. Anything still unnamed stays default-constructed, which is exactly
what ImGui's own `format` / `flags` defaults mean.

The two are also *resolved* at different times, and that decides what "absent"
means:

| | A tool call (run time) | A control (compile time) |
|---|---|---|
| Absent name, optional type | `std::nullopt` | — |
| Absent name, otherwise | `bind_error` | default-constructed value |
| A name that matches nothing | `bind_error` | ignored |