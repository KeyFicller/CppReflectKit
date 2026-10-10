# Widgets from reflection

Walk a struct's members (and its bases) by reflection and draw one widget per field
type; nested structs become collapsible groups. The one idea: **a field's type picks
its widget — by a default control, the reflected enum set, or a `Control<T>` you
specialize — so laying out a form needs no per-member UI code.** Build prerequisites:
[`../ReadMe.md`](../ReadMe.md).

## The deal

```cpp
// What you already have:
struct MyStruct : MyBaseStruct {
    int m_mem_int;
    double m_mem_real;
    Unit m_mem_unit;               // enum     -> a combo of the enumerators
    std::optional<int> m_mem_opt;  // optional -> presence + the inner widget
    std::vector<int> m_mem_vec;    // sequence -> per-element widgets, add/remove
    MyStruct m_mem_other;          // nested   -> a collapsing group, recursed
};
```

```cpp
// All you add — one call, plus an annotation on any field you want to tune:
struct MyForm : MyStruct {
    [[= ui::label{.text = ui::str("Input voltage")}]]                       // human label
    [[= ui::control{.fn = ^^ImGui::SliderInt, .v_min = -10, .v_max = 10}]]  // SliderInt's args
        int m_volts;
    [[= ui::control{.fn = ^^ImGui::DragInt, .v_speed = 0.5f, .v_min = -100, .v_max = 100}]]
        int m_mem_drag;
    [[= ui::control{.fn = ^^ImGui::Checkbox}]] bool m_mem_check;
};

ui::draw(form);   // labelled sliders, checks, enum combos, lists and nested groups,
                  // in declaration order, bases first; no member listed by hand
```

A `label` annotation is the natural one: without it the reflected member name is shown
(`m_volts`); with it, that text replaces the name. The control annotations are
orthogonal — a field can carry both.

An annotation is **per field**. To change a widget for a whole *type*, there are two
routes. For a type the control vocabulary already covers, specialize `default_control<T>`
— it holds the same `control` an annotation does, so the whole form changes at once:

```cpp
namespace ui {
// every int in the form becomes a bounded slider, not a drag
template <> struct default_control<int> { static constexpr control value{ .fn = ^^ImGui::SliderInt, .v_min = -10, .v_max = 10 }; };
}
```

For a type of the app's own, specialize `Control<T>`, whose `draw` may call any ImGui API
and makes the type a leaf rather than a nested group:

```cpp
struct Vec2 { float x; float y; };
namespace ui {
template <> struct Control<Vec2> {
    void draw(Vec2& v) { ImGui::DragFloat2("##vec", &v.x); }   // a leaf, not a nested group
};
}
```

`Vec2` needs the second route: a `default_control` may only name a function from the
vocabulary, and no supported function takes a `Vec2*` (`DragFloat2` wants a `float*`), so a
`Vec2` widget has to be `Control<T>`.

## How the derivation works

**1. The leaf map is a default `ui::control` per type.** Every type the walk draws as a
leaf has a `control` naming the ImGui function to call, so a named field and a bare
value (a sequence's element, an optional's inside) reach ImGui through one path:

```cpp
template <typename T> struct default_control {};
template <> struct default_control<bool> { static constexpr control value{ .fn = ^^ImGui::Checkbox }; };
template <typename T> requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct default_control<T> { static constexpr control value{ .fn = ^^ImGui::DragScalar }; };
template <std::size_t N> struct default_control<char[N]> { static constexpr control value{ .fn = charptr_input_text() }; };
```

Both text types go through `ImGui::InputText`, whose name is overloaded: the
`(label, char* buf, size_t buf_size, ...)` one and the `(label, std::string* str, ...)`
one. A bare `^^ImGui::InputText` picks one of them by implementation choice, and
reflection of a function *type* would carry no parameter names — which the binder needs.
So each overload is pinned by a reference cast to its full signature (default arguments
do not shorten a function type, so every parameter is written out) and turned back into
an entity by `reflect_function`:

```cpp
using StringInputText  = bool(const char*, std::string*, ImGuiInputTextFlags, ImGuiInputTextCallback, void*);
using CharPtrInputText = bool(const char*, char*, size_t, ImGuiInputTextFlags, ImGuiInputTextCallback, void*);
consteval std::meta::info string_input_text()  { return std::meta::reflect_function(static_cast<StringInputText&>(ImGui::InputText)); }
consteval std::meta::info charptr_input_text() { return std::meta::reflect_function(static_cast<CharPtrInputText&>(ImGui::InputText)); }
template <> struct default_control<std::string> { static constexpr control value{ .fn = string_input_text() }; };
```

The pinned entities keep their parameter names, so they flow through the same binder.
The char array decays to the overload's `buf` parameter and `buf_size` is the array's
extent — both from the field's type. The value parameter is named `str` for a
`std::string` and `buf` (plus `buf_size`) for a `char[N]`, the names the binder accepts
beyond `v` / `p_data`.

A type with neither a default nor an annotation is a compile error where it would be
drawn; `Control<T>` is the app's leaf widget for a type the control vocabulary cannot
express.

**2. `draw` picks the branch from the reflected type.** One `if constexpr` chain, the
same shape as the JSON layer's `param_schema`: a specialized control wins, then the
type's default control, then the structured types, then a nested class, else
`static_assert`:

```cpp
template <typename T> void draw(T& _object)
{
    if constexpr (has_control<T>)              Control<T>{}.draw(_object);
    else if constexpr (has_default_control<T>) draw_control<type_control<T>>("##value", _object);
    else if constexpr (is_optional<T>)         draw_optional(_object);
    else if constexpr (is_sequence<T>)         draw_sequence(_object);
    else if constexpr (std::is_enum_v<T>)      draw_enum(_object);
    else if constexpr (std::is_class_v<T>)     { /* collapsible header over a two-column field table */ }
    else static_assert(!sizeof(T), "ui::draw: no widget for T …");
}
```

Where a field's own annotation names a control, that control is read instead of the
type default — `draw_control` takes the source as a parameter, `field_control<Field>` or
`type_control<T>`, and `control_traits` reads the parameters out of whichever it is.

**3. The enum combo is spliced from the declaration.** `enumerators_of` gives both
the names (packed for `ImGui::Combo`) and the values (for the index round-trip), so
the list cannot drift from the enum:

```cpp
template <typename E> consteval std::string enum_names_packed();  // "Celsius\0Fahrenheit\0\0"
template <typename E> consteval auto enum_values();               // the enumerator values
```

**4. `draw_members` walks bases, then fields.** `bases_of` first — an inherited field
is flattened into the owning struct's rows, with no header of its own — then
`nonstatic_data_members_of`. Every struct is one two-column table: labels in a fixed
column, widgets in a stretching one. A nested struct becomes a tree node spanning both
columns and recurses, so its fields are indented rows of the *same* table and the
columns stay aligned at every depth; every other member is a labelled leaf row:

```cpp
template for (constexpr std::meta::info field : fields) {
    if constexpr (is_nested_struct_v<type of field>) { /* TreeNodeEx + draw_members(field) */ }
    else field_row(label, [&] { draw(_object.[:field:]); });   // label cell, widget cell
}
```

**5. `PushID` keeps IDs stable.** Every object and every element is pushed with its
own address, so the same type drawn many times in one window still gets distinct
widget IDs — the usual ImGui requirement, handled once in the walk.

**6. The annotation names the ImGui function; the call is generic.** One annotation,
`ui::control`, whose first member is the function to call (`fn`) and whose remaining
members are that function's parameter names:

```cpp
[[= ui::control{.fn = ^^ImGui::SliderInt, .v_min = -10, .v_max = 10}]] int m_volts;
[[= ui::control{.fn = ^^ImGui::Checkbox}]] bool m_armed;
```

`control` holds the **union** of the parameters across the functions `fn` may name
(`control_functions`, which includes the pinned `InputText` overloads above);
a member the named function does not take is simply ignored. The
members use the widest type the functions accept — `v_min` is an int in `SliderInt` and
a float in `SliderFloat`, so it is held as a `double` — and the call casts each to the
parameter's own type.

Drawing an annotated field is then one generic call, and so is drawing a bare value
through its type's default: `label` and the value come from the walk, `data_type` from
the value's type, and every other argument is bound to the control member of that name —
read out of whichever source the control came from. A parameter no member names stays
default-constructed, which is what ImGui's own `format` / `flags` defaults (`NULL` and
`0`) mean anyway:

```cpp
// the control holds a reflection, so its type is consteval-only and must not reach
// the runtime call: control_traits reads each value in a consteval helper.
constexpr std::meta::info fn = control_traits<Source>::function();
invoke_impl<Source>(_label, _value, std::make_index_sequence<param_count<fn>()>());
// invoke_impl → args tuple of the function's parameter types, bound by name, then
// std::apply(&[:fn:], args)  — the shape of the JSON layer's invoke_with_json, with
// the control standing in for the model's arguments.
```

`Source` is `field_control<Field>` for a field's own annotation and `type_control<T>` for
a type's default, so an annotated field, an unannotated one and a bare value all leave
through this one call.

Two `static_assert`s hold the vocabulary in place: every `control` member must name a
parameter of some supported function (`control_members_are_parameters`), and every use
site's `fn` must be one of them (`is_control_function`). The value's type is checked
against the function too — `fn = ^^ImGui::Checkbox` on an `int` fails, because
`Checkbox` takes a `bool*`.

Because `control` is generic, a member the named function does not take is not caught:
`{.fn = ^^ImGui::SliderInt, .step = 5}` compiles and ignores `step`. That is the price
of one struct instead of one per function.

**7. `label` is the same mechanism, for the display name.** It carries a compile-time
string (`ui::str`), so its type is a specialization (`label<N>`) rather than a fixed
type; `find_annotation_template` matches it the way the JSON layer matches `doc<N>`.
The walk reads it first, and falls back to the reflected member name when there is
none:

```cpp
[[= ui::label{.text = ui::str("Input voltage")}]] int m_volts;   // shown as "Input voltage"
const std::string member_label = field_label<field>();
```

**A reflection query must sit in a constant-evaluated context.** `field_label` is
`consteval` and branches with `if constexpr`; a lookup written as a runtime expression
— `has_annotation_template<^^label, field>() ? … : …` — compiles, but the annotation
comes back empty at run time, so the label silently disappears. Keep the query in an
`if constexpr` (as `field_label` and `is_widget_annotated` do) and out of a `?:`.

## What is derived, per type

`draw` branches on the reflected field type; a leaf goes through its type's default
control, or `Control<T>` when the app specialized one:

| Field type | Drawn as |
|---|---|
| `bool` | a checkbox |
| integer / floating-point | a drag field (`default_control`, overridable) |
| `std::string` | a text field (the pinned `InputText` overload) |
| `char[N]` | a text field (the pinned `buf` / `buf_size` overload) |
| an enum | a combo of the reflected enumerators |
| `std::optional<T>` | a presence checkbox, then `T` when present |
| `std::vector<T>` | per-element widgets with `+ Add` / `x` |
| `std::array<T, N>` | `N` per-element widgets, no add/remove |
| a class with no `Control<T>` | a `CollapsingHeader`, recursed into |
| a base class | its members, inlined in the derived object |
| a field with a **control annotation** | that control, driven by the annotation's parameters |
| a type with a **`Control<T>`** | that control, as a leaf rather than a nested group |
| anything else | a `static_assert` at the point of use |

The recursion is the whole layout: a struct with a nested struct draws a group inside
a group, to any depth, from the declarations alone.

## Consuming it

The library is `lib/inc/ui/reflection_widgets.h`; the demo data and the annotated
fields are `test/ui/scaffold/reflection_widgets.h`. Overriding a widget type-wide
instead of per field is a `default_control<T>` when the control vocabulary covers it, or
a `Control<T>` specialization when it does not. Opening a window
means the target cannot run unattended, so `ui` is registered with CTest but
**disabled** — run it directly:

```sh
cmake --build build --target ui_reflection
./build/ui_reflection        # GLFW + OpenGL 3 window, ImGui demo + the reflected form
```

Because the target cannot run under CTest, the annotation path is verified at compile
time instead: `test/ui/test_reflection_widgets.cpp` asserts that each annotation is
read and its values come through (`ui::annotation_value<…>().v_min == -10`), that the
annotation names the function it should (`…().fn == ^^ImGui::SliderInt`), and that the
vocabulary still holds (`control_members_are_parameters`, `is_control_function`), so a
typo in `control` or an unsupported `fn` is rejected at build time.

`ui_reflection` (`test/ui/main.cpp`) calls `ui_reflection()`, which passes a draw
callback to `run_imgui_demo_window` in the backend scaffold
(`test/ui/scaffold/imgui_backend_glfw_opengl3.cpp`): the backend owns the frame loop,
the demo owns the one `ui::draw(data)` call inside it.
