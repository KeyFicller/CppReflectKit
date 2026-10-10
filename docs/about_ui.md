# Widgets from reflection

Walk a struct's members (and its bases) by reflection and draw one widget per field
type; nested structs become collapsible groups. The one idea: **a field's type picks
its widget — by a built-in control, the reflected enum set, or a `Control<T>` you
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
    [[= ui::label{.text = ui::str("Input voltage")}]]                // human label
    [[= ui::slider{.v_min = -10, .v_max = 10}]] int m_volts;         // SliderInt's args
    [[= ui::drag{.v_speed = 0.5f, .v_min = -100, .v_max = 100}]] int m_mem_drag;
    [[= ui::checkbox{}]] bool m_mem_check;
};

ui::draw(form);   // labelled sliders, checks, enum combos, lists and nested groups,
                  // in declaration order, bases first; no member listed by hand
```

A `label` annotation is the natural one: without it the reflected member name is shown
(`m_volts`); with it, that text replaces the name. The control annotations are
orthogonal — a field can carry both.

Override a built-in type-wide only where the app wants something specific — here a
slider instead of the default drag field:

```cpp
namespace ui {
template <> struct Control<int> {
    void draw(int& value) { ImGui::SliderInt("##slider", &value, 0, 100); }
};
}
```

## How the derivation works

**1. `Control<T>` is the leaf map.** The header ships controls for `bool`, every
arithmetic type and `std::string`; `Control<T>` is the override point, and a type
that is neither built-in nor a reflected class is a compile error where it would be
drawn:

```cpp
template <typename T> struct Control {};
template <typename T> requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct Control<T> { void draw(T& v) { ImGui::DragScalar("##scalar", imgui_data_type<T>(), &v); } };
```

**2. `draw` picks the branch from the reflected type.** One `if constexpr` chain, the
same shape as the JSON layer's `param_schema`: a specialized control wins, then the
structured types, then a nested class, else `static_assert`:

```cpp
template <typename T> void draw(T& _object)
{
    if constexpr (has_control<T>)         Control<T>{}.draw(_object);
    else if constexpr (is_optional<T>)    draw_optional(_object);
    else if constexpr (is_sequence<T>)    draw_sequence(_object);
    else if constexpr (std::is_enum_v<T>) draw_enum(_object);
    else if constexpr (std::is_class_v<T>) { /* collapsible header over a two-column field table */ }
    else static_assert(!sizeof(T), "ui::draw: no Control<T> …");
}
```

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

**6. An annotation overrides the widget per field.** `find_annotation` scans
`annotations_of(field)` for a ui control type, and the annotation's members are the
control's parameters. The member **names** are tied to the ImGui function the control
calls, and a `static_assert` reflects that function to enforce it — so a parameter
cannot drift from the API:

```cpp
struct slider { int v_min = 0; int v_max = 100; };   // named after SliderInt's params
static_assert(params_name_function_arguments<^^slider, ^^ImGui::SliderInt>(), …);

// in the walk: an annotated field uses its control, the rest fall back to the type
if constexpr (is_widget_annotated<field>()) draw_annotated<field>(_object.[:field:]);
```

The annotation is read entirely at compile time (`meta::extract`), so a misspelled key
is a build error, not a silently ignored option.

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

`draw` branches on the reflected field type; a leaf consults `Control<T>`:

| Field type | Drawn as |
|---|---|
| `bool` | a checkbox |
| integer / floating-point | a drag field (`Control<T>`, overridable) |
| `std::string` | a text field |
| an enum | a combo of the reflected enumerators |
| `std::optional<T>` | a presence checkbox, then `T` when present |
| `std::vector<T>` | per-element widgets with `+ Add` / `x` |
| `std::array<T, N>` | `N` per-element widgets, no add/remove |
| a class with no `Control<T>` | a `CollapsingHeader`, recursed into |
| a base class | its members, inlined in the derived object |
| a field with a **control annotation** | that control, driven by the annotation's parameters |
| anything else | a `static_assert` at the point of use |

The recursion is the whole layout: a struct with a nested struct draws a group inside
a group, to any depth, from the declarations alone.

## Consuming it

The library is `lib/inc/ui/reflection_widgets.h`; the demo data, one type-wide override
and the annotated fields are `test/ui/scaffold/reflection_widgets.h`. Opening a window
means the target cannot run unattended, so `ui` is registered with CTest but
**disabled** — run it directly:

```sh
cmake --build build --target ui_reflection
./build/ui_reflection        # GLFW + OpenGL 3 window, ImGui demo + the reflected form
```

Because the target cannot run under CTest, the annotation path is verified at compile
time instead: `test/ui/test_reflection_widgets.cpp` asserts that each annotation is
read and its values come through (`ui::annotation_value<…>().v_min == -10`), and the
header's `static_assert` proves a bad key name is rejected.

`ui_reflection` (`test/ui/main.cpp`) calls `ui_reflection()`, which passes a draw
callback to `run_imgui_demo_window` in the backend scaffold
(`test/ui/scaffold/imgui_backend_glfw_opengl3.cpp`): the backend owns the frame loop,
the demo owns the one `ui::draw(data)` call inside it.
