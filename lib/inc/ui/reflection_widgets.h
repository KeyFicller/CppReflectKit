#pragma once

// Reflection-driven ImGui widgets: walk a struct's members (and its bases) by
// reflection and draw one widget per field type, recursing into nested structs.
// The same type matrix the JSON layer covers has a built-in widget -- bool,
// integers, floats, strings, enums, optional, vector and array -- so a reflected
// form works with no per-type UI code. Control<T> overrides any one of them, or
// gives a user type a leaf widget instead of the default "nested struct" group.
// A field may also carry a widget annotation -- [[= ui::control{.fn = ^^ImGui::SliderInt, .v_min = 0, .v_max = 50}]]
// -- to override its control per field. The annotation names the ImGui function
// to call and carries that function's parameters as named members, checked by
// reflection below. [[= ui::hidden{}]] leaves a field out of the form. See
// docs/about_ui.md.

#include <array>
#include <cstddef>
#include <memory>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace ui {

// --- type traits ------------------------------------------------------------

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T> struct is_sequence : std::false_type {};
template <typename T, typename A> struct is_sequence<std::vector<T, A>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = false;
};
template <typename T, std::size_t N> struct is_sequence<std::array<T, N>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = true;
    static constexpr std::size_t extent = N;
};

template <typename T>
inline constexpr bool is_string_v = std::is_same_v<std::remove_cv_t<T>, std::string>;

// --- leaf controls ----------------------------------------------------------
// Control<T> is the extension point for a leaf widget of the app's own: specialize
// it to give a type one instead of the default nested-struct group. The library's
// own widget for a type is not here but in `default_control` below -- the same
// `ui::control` an annotation carries, so a named field and a bare value (a
// sequence's element, an optional's inside) are drawn through one path.

template <typename T> struct Control {};

/// The ImGui scalar tag for a given arithmetic type.
template <typename T> constexpr ImGuiDataType imgui_data_type()
{
    if constexpr (std::is_same_v<T, float>) {
        return ImGuiDataType_Float;
    } else if constexpr (std::is_same_v<T, double>) {
        return ImGuiDataType_Double;
    } else if constexpr (std::is_signed_v<T>) {
        if constexpr (sizeof(T) == 1) return ImGuiDataType_S8;
        else if constexpr (sizeof(T) == 2) return ImGuiDataType_S16;
        else if constexpr (sizeof(T) == 4) return ImGuiDataType_S32;
        else return ImGuiDataType_S64;
    } else {
        if constexpr (sizeof(T) == 1) return ImGuiDataType_U8;
        else if constexpr (sizeof(T) == 2) return ImGuiDataType_U16;
        else if constexpr (sizeof(T) == 4) return ImGuiDataType_U32;
        else return ImGuiDataType_U64;
    }
}

template <typename T>
concept has_control = requires(T& _value) { Control<T>{}.draw(_value); };

/// A class the walk should present as a collapsible group rather than a leaf:
/// not a string, optional or sequence, and with no Control<T> of its own.
template <typename T>
inline constexpr bool is_nested_struct_v =
    std::is_class_v<T> && !is_string_v<T> && !is_optional<T>::value &&
    !is_sequence<T>::value && !has_control<T>;

// --- per-field widget annotations -------------------------------------------
// An annotation on a field overrides the widget its type would otherwise get and
// carries the control's parameters as named members. The member names are pinned
// to the ImGui function the control calls: the static_assert below reflects that
// function and rejects any name it does not take, so a parameter cannot drift
// from the API. Control<T> is the type-level default; an annotation is the
// per-field override. See docs/about_ui.md.

/// First annotation on `_e` whose type is exactly `A` (a non-template type).
template <std::meta::info A>
consteval std::meta::info find_annotation(std::meta::info _e)
{
    for (std::meta::info a : std::meta::annotations_of(_e)) {
        if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(a)), A)) {
            return a;
        }
    }
    return {};
}

/// First annotation on `_e` whose type is an instantiation of the template `Tmpl`,
/// for annotations that carry a compile-time string (their length is a template
/// argument, so the type is a specialization rather than a fixed type).
template <std::meta::info Tmpl>
consteval std::meta::info find_annotation_template(std::meta::info _e)
{
    for (std::meta::info a : std::meta::annotations_of(_e)) {
        const std::meta::info t = std::meta::remove_const(std::meta::type_of(a));
        if (std::meta::has_template_arguments(t) && std::meta::template_of(t) == Tmpl) {
            return a;
        }
    }
    return {};
}

template <std::meta::info A, std::meta::info E>
consteval bool has_annotation()
{
    return find_annotation<A>(E) != std::meta::info{};
}

template <std::meta::info Tmpl, std::meta::info E>
consteval bool has_annotation_template()
{
    return find_annotation_template<Tmpl>(E) != std::meta::info{};
}

/// The value of `A`'s annotation on `E`.
template <std::meta::info A, std::meta::info E>
consteval auto annotation_value()
{
    return std::meta::extract<typename[:std::meta::type_of(find_annotation<A>(E)):]>(find_annotation<A>(E));
}

/// A compile-time string an annotation can hold. An annotation value must be
/// structural, so the text is a fixed-length char array, e.g. str<8>{"Voltage"}.
template <std::size_t N>
struct str {
    char data[N]{};
    constexpr str() = default;
    constexpr str(const char (&s)[N])
    {
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = s[i];
        }
    }
};
template <std::size_t N>
str(const char (&)[N]) -> str<N>;

/// A field's display label, replacing the reflected member name:
///   [[= ui::label{ .text = ui::str("Input voltage") }]] int m_volts;
template <std::size_t N = 1>
struct label {
    str<N> text;
};

/// A field left out of the form entirely:
///   [[= ui::hidden{}]] int m_internal;
struct hidden {};

/// The text of a `label` annotation on `E`, or "" when there is none.
template <std::meta::info E>
consteval std::string annotation_label()
{
    if constexpr (has_annotation_template<^^label, E>()) {
        constexpr auto cfg =
            std::meta::extract<typename[:std::meta::type_of(find_annotation_template<^^label>(E)):]>(
                find_annotation_template<^^label>(E));
        return std::string(std::string_view(cfg.text.data));
    } else {
        return {};
    }
}

/// A field's label: its `label` annotation text, else the reflected member name.
/// Computed through `if constexpr` on purpose -- a reflection query is only
/// reliable in a constant-evaluated context, so the annotation lookup must not
/// sit in a runtime expression.
template <std::meta::info Field>
consteval std::string field_label()
{
    if constexpr (has_annotation_template<^^label, Field>()) {
        return annotation_label<Field>();
    } else {
        constexpr auto name = std::meta::display_string_of(Field);
        return std::string(name.data(), name.size());
    }
}

// One annotation, `ui::control`, names the function to call (`fn`) and carries that
// function's parameters as named members. `label` and the value come from
// the walk, `data_type` from the field's type. It holds the union of the
// parameters across every supported function, so a member the named function does
// not take is ignored -- set the ones it does.
//   SliderInt(label, v, v_min, v_max, format, flags)
//   SliderFloat(label, v, v_min, v_max, format, flags)
//   SliderScalar(label, data_type, p_data, p_min, p_max, format, flags)
//   DragInt(label, v, v_speed, v_min, v_max, format, flags)
//   DragFloat(label, v, v_speed, v_min, v_max, format, flags)
//   DragScalar(label, data_type, p_data, v_speed, p_min, p_max, format, flags)
//   InputInt(label, v, step, step_fast, flags)
//   InputDouble(label, v, step, step_fast, format, flags)
//   InputScalar(label, data_type, p_data, p_step, p_step_fast, format, flags)
//   Checkbox(label, v)
//   InputText(label, str, flags, callback, user_data)   -- the std::string overload,
//                                    pinned by a cast signature
//   InputText(label, buf, buf_size, flags, callback, user_data)   -- the char array
//                                    overload, pinned the same way
// The members are declared in an order that matches the parameter order of every
// supported function, because designated initializers must be written in
// declaration order. They take the widest type the functions accept: `v_min` is
// an int in SliderInt and a float in SliderFloat, so it is held as a double. A
// `*Scalar` bound is a `const void*` to a value of the field's own type, so its
// double is cast to that type at the call.
struct control {
    std::meta::info fn;           // the function to call (mandatory, first)
    float v_speed = 1.0f;         // Drag*        -- before v_min, as in DragInt
    double v_min = 0.0;           // Slider* / Drag*
    double v_max = 0.0;
    double step = 1.0;            // InputInt     -- its own step / step_fast defaults
    double step_fast = 100.0;
    double p_min = 0.0;           // *Scalar
    double p_max = 0.0;
    double p_step = 0.0;          // InputScalar
    double p_step_fast = 0.0;
};

/// ImGui::InputText is overloaded, and reflection reaches neither overload by the
/// plain `^^ImGui::InputText`: that names one of them by implementation choice (here
/// the `(label, char* buf, size_t buf_size, ...)` one), and a function *type* would
/// carry no parameter names. A reference cast pins an overload by its full signature
/// -- default arguments do not shorten a function type, so every parameter is written
/// out -- and reflect_function turns that back into the entity, names and all.
using StringInputText = bool(const char*, std::string*, ImGuiInputTextFlags, ImGuiInputTextCallback, void*);
consteval std::meta::info string_input_text()
{
    return std::meta::reflect_function(static_cast<StringInputText&>(ImGui::InputText));
}

using CharPtrInputText = bool(const char*, char*, size_t, ImGuiInputTextFlags, ImGuiInputTextCallback, void*);
consteval std::meta::info charptr_input_text()
{
    return std::meta::reflect_function(static_cast<CharPtrInputText&>(ImGui::InputText));
}

/// The functions `ui::control` can drive: each takes a label and a value parameter
/// named `v`, `p_data`, `str` or `buf` (a char array also gives `buf_size`), and
/// `fn` must be one of them.
inline constexpr std::array control_functions{
    ^^ImGui::SliderInt,  ^^ImGui::SliderFloat, ^^ImGui::SliderScalar,
    ^^ImGui::DragInt,    ^^ImGui::DragFloat,   ^^ImGui::DragScalar,
    ^^ImGui::InputInt,   ^^ImGui::InputDouble, ^^ImGui::InputScalar,
    ^^ImGui::Checkbox,   string_input_text(),  charptr_input_text(),
};

template <std::meta::info Fn>
consteval bool is_control_function()
{
    bool found = false;
    template for (constexpr std::meta::info f : control_functions) {
        if constexpr (Fn == f) {
            found = true;
        }
    }
    return found;
}

/// Every member `control` can name must be a parameter of some supported
/// function, so a typo in the struct is caught here rather than silently ignored
/// at every use site.
consteval bool control_members_are_parameters()
{
    for (std::meta::info member :
         std::meta::nonstatic_data_members_of(^^control, std::meta::access_context::unchecked())) {
        const std::string_view name = std::meta::identifier_of(member);
        if (name == "fn") {
            continue;
        }
        bool named = false;
        for (std::meta::info f : control_functions) {
            for (std::meta::info p : std::meta::parameters_of(f)) {
                if (name == std::meta::identifier_of(p)) {
                    named = true;
                }
            }
        }
        if (!named) {
            return false;
        }
    }
    return true;
}
static_assert(control_members_are_parameters(),
              "every ui::control member must name a parameter of a supported ImGui function");

/// The control a value's type is drawn with when no annotation names one. Written
/// the same way an annotation is -- a `ui::control` naming a function -- so the
/// walk and a bare value reach ImGui through one path.
template <typename T> struct default_control {};
template <> struct default_control<bool> {
    static constexpr control value{ .fn = ^^ImGui::Checkbox };
};
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct default_control<T> {
    static constexpr control value{ .fn = ^^ImGui::DragScalar };
};
template <> struct default_control<std::string> {
    static constexpr control value{ .fn = string_input_text() };
};
/// A char array is the other InputText: the buffer itself plus its size, which the
/// binder takes from the array's extent.
template <std::size_t N> struct default_control<char[N]> {
    static constexpr control value{ .fn = charptr_input_text() };
};

template <typename T>
concept has_default_control = requires { default_control<T>::value; };

/// Where a control's parameter values come from: a field's own annotation, or the
/// default for a value's type (`draw` reaches a value with no field around it).
template <std::meta::info Field> struct field_control {};
template <typename T> struct type_control {};

template <typename Source> struct control_traits;
template <std::meta::info Field> struct control_traits<field_control<Field>> {
    static consteval std::meta::info function() { return annotation_value<^^control, Field>().fn; }
    template <std::meta::info Member> static consteval auto member()
    {
        return annotation_value<^^control, Field>().[:Member:];
    }
};
template <typename T> struct control_traits<type_control<T>> {
    static consteval std::meta::info function() { return default_control<T>::value.fn; }
    template <std::meta::info Member> static consteval auto member()
    {
        return default_control<T>::value.[:Member:];
    }
};

// --- calling a control -------------------------------------------------------
// Parameter introspection, as in the JSON layer's tool call.

template <std::meta::info Fn>
consteval std::size_t param_count()
{
    return std::meta::parameters_of(Fn).size();
}

template <std::meta::info Fn, std::size_t I>
consteval std::meta::info param_at()
{
    return std::meta::parameters_of(Fn)[I];
}

template <std::meta::info Fn, std::size_t I>
using param_type_at = std::remove_cvref_t<typename[:std::meta::type_of(param_at<Fn, I>()):]>;

/// The member of annotation `A` naming parameter `P`, or {} when the annotation
/// does not name it and the parameter keeps its zero value.
template <std::meta::info A, std::meta::info P>
consteval std::meta::info control_argument_of()
{
    for (std::meta::info m : std::meta::nonstatic_data_members_of(A, std::meta::access_context::unchecked())) {
        if (std::meta::identifier_of(m) == std::meta::identifier_of(P)) {
            return m;
        }
    }
    return {};
}

/// Fills argument `I` of `Fn`. `label` and the value come from the walk, the data
/// type from the value's type. Every other parameter comes from the control's
/// member of that name, read out of the consteval-only `control` as a plain value;
/// one no member names stays default-constructed, which is what ImGui's own
/// `format` / `flags` defaults (`NULL` and `0`) mean anyway.
template <typename Source, std::meta::info Fn, std::size_t I, typename T, typename Storage>
void bind_argument(Storage& _storage, auto& _args, const char* _label, T& _value)
{
    using PT = param_type_at<Fn, I>;
    constexpr std::string_view name = std::define_static_string(std::meta::identifier_of(param_at<Fn, I>()));
    constexpr std::meta::info member = control_argument_of<^^control, param_at<Fn, I>()>();
    if constexpr (name == "label") {
        std::get<I>(_args) = _label;
    } else if constexpr (name == "v" || name == "p_data" || name == "str") {
        static_assert(std::is_same_v<std::remove_cv_t<std::remove_pointer_t<PT>>, T> ||
                          std::is_void_v<std::remove_cv_t<std::remove_pointer_t<PT>>>,
                      "the controlled value's type must be the one this function takes");
        std::get<I>(_args) = static_cast<PT>(std::addressof(_value));
    } else if constexpr (name == "buf") {
        // The char array itself, which decays to the char* the function takes.
        static_assert(std::is_array_v<T>, "a buf parameter wants a char array");
        std::get<I>(_args) = static_cast<PT>(_value);
    } else if constexpr (name == "buf_size") {
        // ...and its extent, the array being the buffer.
        static_assert(std::is_array_v<T>, "a buf_size parameter comes from a char array's extent");
        std::get<I>(_args) = static_cast<PT>(std::extent_v<T>);
    } else if constexpr (name == "data_type") {
        std::get<I>(_args) = imgui_data_type<T>();
    } else if constexpr (member != std::meta::info{}) {
        constexpr auto raw = control_traits<Source>::template member<member>();
        if constexpr (std::is_pointer_v<PT>) {
            // A *Scalar bound: the function reads a value of the value's own type
            // through the pointer, so that value lives in `_storage` for the call.
            static_assert(std::is_void_v<std::remove_cv_t<std::remove_pointer_t<PT>>>,
                          "a bound pointer parameter must be a void* to the value's type");
            _storage[I] = static_cast<T>(raw);
            std::get<I>(_args) = static_cast<PT>(std::addressof(_storage[I]));
        } else {
            std::get<I>(_args) = static_cast<PT>(raw);
        }
    }
}

template <typename Source, typename T, std::size_t... Is>
void invoke_impl(const char* _label, T& _value, std::index_sequence<Is...>)
{
    constexpr std::meta::info fn = control_traits<Source>::function();
    std::tuple<param_type_at<fn, Is>...> args{};
    // One slot per parameter, for a bound pointer parameter to point at.
    std::array<T, sizeof...(Is)> storage{};
    (bind_argument<Source, fn, Is>(storage, args, _label, _value), ...);
    std::apply(&[:fn:], args);
}

/// Draws a value through a control: the function its annotation (or its type's
/// default) named, with the parameters that control carries. `_label` is the hidden
/// ImGui label -- the visible one is drawn by the walk, in its own column.
template <typename Source, typename T>
void draw_control(const char* _label, T& _value)
{
    constexpr std::meta::info fn = control_traits<Source>::function();
    static_assert(is_control_function<fn>(), "ui::control: fn must be a supported function");
    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    invoke_impl<Source>(_label, _value, std::make_index_sequence<param_count<fn>()>());
    ImGui::PopID();
}

/// Whether the field carries a ui::control annotation.
template <std::meta::info Field>
consteval bool is_widget_annotated()
{
    return has_annotation<^^control, Field>();
}

/// Whether the field is marked [[= ui::hidden{}]] and must not be drawn.
template <std::meta::info Field>
consteval bool is_hidden()
{
    return has_annotation<^^hidden, Field>();
}

// --- the enum combo ---------------------------------------------------------
// An enum is spliced from the reflected enumerators, so the combo always lists
// the real set: no hand-written table to drift from the declaration.

template <typename E>
consteval std::string enum_names_packed()
{
    std::string out;
    for (std::meta::info e : std::meta::enumerators_of(^^E)) {
        out += std::string(std::meta::identifier_of(e));
        out.push_back('\0');
    }
    out.push_back('\0');
    return out;
}

template <typename E>
consteval auto enum_values()
{
    constexpr auto infos = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::underlying_type_t<E>, infos.size()> out{};
    std::size_t i = 0;
    template for (constexpr std::meta::info e : infos) {
        out[i++] = static_cast<std::underlying_type_t<E>>([:e:]);
    }
    return out;
}

// --- the reflection walk ----------------------------------------------------

template <typename T> void draw(T& _object);
template <typename T> void draw_members(T& _object);

/// enum: a combo box listing the reflected enumerators.
template <typename E>
void draw_enum(E& _value)
{
    constexpr std::string packed = enum_names_packed<E>();
    constexpr auto values = enum_values<E>();
    using underlying = std::underlying_type_t<E>;

    int current = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (static_cast<underlying>(_value) == values[i]) {
            current = static_cast<int>(i);
            break;
        }
    }

    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::Combo("##enum", &current, packed.c_str(), static_cast<int>(values.size()))) {
        _value = static_cast<E>(values[static_cast<std::size_t>(current)]);
    }
    ImGui::PopID();
}

/// optional: a presence checkbox, then the wrapped value when present.
template <typename T>
void draw_optional(std::optional<T>& _value)
{
    ImGui::PushID(std::addressof(_value));
    bool present = _value.has_value();
    if (ImGui::Checkbox("##present", &present)) {
        if (present) _value.emplace();
        else _value.reset();
    }
    ImGui::SameLine();
    if (_value) {
        draw(*_value);
    } else {
        ImGui::TextDisabled("(empty)");
    }
    ImGui::PopID();
}

/// vector / array: each element through draw(). A vector also gets add / remove;
/// an array has a fixed length and gets neither.
template <typename V>
void draw_sequence(V& _value)
{
    ImGui::PushID(std::addressof(_value));

    bool erase = false;
    std::size_t erase_at = 0;
    for (std::size_t i = 0; i < _value.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        // The remove button goes first, so the element widget can take the
        // rest of the row.
        if constexpr (!is_sequence<V>::fixed_size) {
            if (ImGui::SmallButton("x")) {
                erase = true;
                erase_at = i;
            }
            ImGui::SameLine();
        }
        draw(_value[i]);
        ImGui::PopID();
    }
    if constexpr (!is_sequence<V>::fixed_size) {
        if (erase) {
            _value.erase(_value.begin() + static_cast<std::ptrdiff_t>(erase_at));
        }
        if (ImGui::SmallButton("+ Add")) {
            _value.emplace_back();
        }
    }

    ImGui::PopID();
}

// --- the field table ----------------------------------------------------------
// A struct is laid out as one two-column table: labels in a fixed column, widgets
// in a stretching one, so every widget starts at the same x and fills the row.
// Nested structs are tree nodes spanning both columns; their fields are indented
// rows of the *same* table, which keeps the columns aligned at every depth.

/// Opens the field table. Returns false when the table is clipped away.
inline bool begin_field_table()
{
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##fields", 2, flags)) return false;
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// One leaf row: the label, then the widget in the value column.
template <typename Fn>
void field_row(const std::string& _label, Fn&& _widget)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_label.c_str());
    ImGui::TableNextColumn();
    _widget();
}

/// Draws one value: a class as a collapsible group over its field table, an
/// enum, optional or sequence through its built-in widget, anything else through
/// its type's default control -- or Control<T>, if the app gave it one.
template <typename T>
void draw(T& _object)
{
    if constexpr (has_control<T>) {
        Control<T>{}.draw(_object);
    } else if constexpr (has_default_control<T>) {
        draw_control<type_control<T>>("##value", _object);
    } else if constexpr (is_optional<T>::value) {
        draw_optional(_object);
    } else if constexpr (is_sequence<T>::value) {
        draw_sequence(_object);
    } else if constexpr (std::is_enum_v<T>) {
        draw_enum(_object);
    } else if constexpr (std::is_class_v<T>) {
        ImGui::PushID(std::addressof(_object));
        const std::string identifier(std::meta::identifier_of(^^T));
        if (ImGui::CollapsingHeader(identifier.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (begin_field_table()) {
                draw_members(_object);
                ImGui::EndTable();
            }
        }
        ImGui::PopID();
    } else {
        static_assert(!sizeof(T),
                      "ui::draw: no Control<T> for this type, and it is not an enum, "
                      "optional, sequence or reflected class");
    }
}

/// Emits the rows of `_object` into the open field table: first its bases'
/// fields (flattened, no header of their own), then each member. A nested
/// struct member becomes a tree node whose fields are indented rows; every
/// other member is a labelled leaf drawn through draw().
template <typename T>
void draw_members(T& _object)
{
    if constexpr (std::meta::is_class_type(^^T)) {

        constexpr auto bases =
            std::define_static_array(std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info base : bases) {
            constexpr std::meta::info bt = std::meta::dealias(std::meta::type_of(base));
            if constexpr (std::meta::is_class_type(bt)) {
                using base_type = typename[:bt:];
                draw_members(static_cast<base_type&>(_object));
            }
        }

        constexpr auto fields =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty = std::meta::dealias(std::meta::type_of(field));
            if constexpr (is_hidden<field>()) {
                continue;   // [[= ui::hidden{}]]: this field is not drawn
            }
            // field_label runs the annotation lookup in a constant context; a
            // reflection query in a runtime expression is not reliable.
            const std::string member_label = field_label<field>();

            if constexpr (is_widget_annotated<field>()) {
                field_row(member_label, [&] { draw_control<field_control<field>>("##value", _object.[:field:]); });
            } else if constexpr (is_nested_struct_v<typename[:fty:]>) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool open = ImGui::TreeNodeEx(std::addressof(_object.[:field:]),
                                                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAllColumns,
                                                    "%s", member_label.c_str());
                if (open) {
                    draw_members(_object.[:field:]);
                    ImGui::TreePop();
                }
            } else {
                field_row(member_label, [&] { draw(_object.[:field:]); });
            }
        }
    }
}

} // namespace ui
