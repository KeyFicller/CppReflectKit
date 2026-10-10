#pragma once

// Reflection-driven ImGui widgets: walk a struct's members (and its bases) by
// reflection and draw one widget per field type, recursing into nested structs.
// The same type matrix the JSON layer covers has a built-in widget -- bool,
// integers, floats, strings, enums, optional, vector and array -- so a reflected
// form works with no per-type UI code. Control<T> overrides any one of them, or
// gives a user type a leaf widget instead of the default "nested struct" group.
// A field may also carry a widget annotation -- [[= ui::slider{.v_min = 0, .v_max = 50}]]
// -- to override its control per field; the annotation's member names are pinned
// to the ImGui function the control calls. See docs/about_ui.md.

#include <array>
#include <cstddef>
#include <memory>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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
// Control<T> is the override point. The header ships one for bool, every
// arithmetic type and std::string; specialize it to change one of those or to
// give a user type a leaf widget instead of the default nested-struct group.

template <typename T> struct Control {};

template <> struct Control<bool> {
    void draw(bool& _value)
    {
        ImGui::PushID(std::addressof(_value));
        ImGui::Checkbox("##bool", &_value);
        ImGui::PopID();
    }
};

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
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct Control<T> {
    void draw(T& _value)
    {
        ImGui::PushID(std::addressof(_value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::DragScalar("##scalar", imgui_data_type<T>(), &_value);
        ImGui::PopID();
    }
};

template <> struct Control<std::string> {
    void draw(std::string& _value)
    {
        ImGui::PushID(std::addressof(_value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##text", &_value);
        ImGui::PopID();
    }
};

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

/// The parameter names of `Fn`, e.g. {"label","v","v_min","v_max","format","flags"}.
template <std::meta::info Fn>
consteval auto param_names()
{
    constexpr auto params = std::define_static_array(std::meta::parameters_of(Fn));
    std::array<std::string_view, params.size()> out{};
    std::size_t i = 0;
    template for (constexpr std::meta::info p : params) {
        constexpr auto id = std::meta::identifier_of(p);
        out[i++] = std::string_view(id.data(), id.size());
    }
    return out;
}

/// Every member of annotation type `A` names a parameter of `Fn`.
template <std::meta::info A, std::meta::info Fn>
consteval bool params_name_function_arguments()
{
    constexpr auto names = param_names<Fn>();
    constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(A, std::meta::access_context::unchecked()));
    template for (constexpr std::meta::info m : members) {
        constexpr auto id = std::meta::identifier_of(m);
        constexpr std::string_view name(id.data(), id.size());
        bool matched = false;
        for (std::string_view n : names) {
            if (n == name) {
                matched = true;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

// The control's parameters, as named members. The names mirror the ImGui
// function each control calls, so `[[= ui::slider{.v_min = 0, .v_max = 50}]]`
// reads as SliderInt's own arguments:
//   SliderInt(label, v, v_min, v_max, format, flags)
//   DragInt(label, v, v_speed, v_min, v_max, format, flags)
//   InputInt(label, v, step, step_fast, flags)
//   Checkbox(label, v)
struct slider {
    int v_min = 0;
    int v_max = 100;
};
struct drag {
    float v_speed = 1.0f;
    int v_min = 0;
    int v_max = 0;
};
struct input {
    int step = 1;
    int step_fast = 100;
};
struct checkbox {};

static_assert(params_name_function_arguments<^^slider, ^^ImGui::SliderInt>(),
              "ui::slider's members must name parameters of ImGui::SliderInt");
static_assert(params_name_function_arguments<^^drag, ^^ImGui::DragInt>(),
              "ui::drag's members must name parameters of ImGui::DragInt");
static_assert(params_name_function_arguments<^^input, ^^ImGui::InputInt>(),
              "ui::input's members must name parameters of ImGui::InputInt");
static_assert(params_name_function_arguments<^^checkbox, ^^ImGui::Checkbox>(),
              "ui::checkbox's members must name parameters of ImGui::Checkbox");

/// The annotated controls, each calling the ImGui entry point for the field's
/// type with the parameters the annotation supplied.
template <typename T>
void draw_control(T& _value, const slider& _cfg)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "ui::slider: an arithmetic field");
    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if constexpr (std::is_same_v<T, int>) {
        ImGui::SliderInt("##slider", &_value, _cfg.v_min, _cfg.v_max);
    } else if constexpr (std::is_same_v<T, float>) {
        ImGui::SliderFloat("##slider", &_value, static_cast<float>(_cfg.v_min), static_cast<float>(_cfg.v_max));
    } else {
        const T lo = static_cast<T>(_cfg.v_min);
        const T hi = static_cast<T>(_cfg.v_max);
        ImGui::SliderScalar("##slider", imgui_data_type<T>(), &_value, &lo, &hi);
    }
    ImGui::PopID();
}

template <typename T>
void draw_control(T& _value, const drag& _cfg)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "ui::drag: an arithmetic field");
    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if constexpr (std::is_same_v<T, int>) {
        ImGui::DragInt("##drag", &_value, _cfg.v_speed, _cfg.v_min, _cfg.v_max);
    } else if constexpr (std::is_same_v<T, float>) {
        ImGui::DragFloat("##drag", &_value, _cfg.v_speed, static_cast<float>(_cfg.v_min), static_cast<float>(_cfg.v_max));
    } else {
        const T lo = static_cast<T>(_cfg.v_min);
        const T hi = static_cast<T>(_cfg.v_max);
        ImGui::DragScalar("##drag", imgui_data_type<T>(), &_value, _cfg.v_speed, &lo, &hi);
    }
    ImGui::PopID();
}

template <typename T>
void draw_control(T& _value, const input& _cfg)
{
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, "ui::input: an arithmetic field");
    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if constexpr (std::is_same_v<T, int>) {
        ImGui::InputInt("##input", &_value, _cfg.step, _cfg.step_fast);
    } else if constexpr (std::is_same_v<T, double>) {
        ImGui::InputDouble("##input", &_value, _cfg.step, _cfg.step_fast);
    } else {
        const T step = static_cast<T>(_cfg.step);
        const T step_fast = static_cast<T>(_cfg.step_fast);
        ImGui::InputScalar("##input", imgui_data_type<T>(), &_value, &step, &step_fast);
    }
    ImGui::PopID();
}

inline void draw_control(bool& _value, const checkbox&)
{
    ImGui::PushID(std::addressof(_value));
    ImGui::Checkbox("##checkbox", &_value);
    ImGui::PopID();
}

template <typename T>
void draw_control(T&, const checkbox&)
{
    static_assert(!sizeof(T), "ui::checkbox: a bool field");
}

template <std::meta::info Field>
consteval bool is_widget_annotated()
{
    return has_annotation<^^slider, Field>() || has_annotation<^^drag, Field>() ||
           has_annotation<^^input, Field>() || has_annotation<^^checkbox, Field>();
}

/// Draws a field through whichever control its annotation named.
template <std::meta::info Field, typename T>
void draw_annotated(T& _value)
{
    if constexpr (has_annotation<^^slider, Field>()) {
        draw_control(_value, annotation_value<^^slider, Field>());
    } else if constexpr (has_annotation<^^drag, Field>()) {
        draw_control(_value, annotation_value<^^drag, Field>());
    } else if constexpr (has_annotation<^^input, Field>()) {
        draw_control(_value, annotation_value<^^input, Field>());
    } else if constexpr (has_annotation<^^checkbox, Field>()) {
        draw_control(_value, annotation_value<^^checkbox, Field>());
    }
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
/// enum, optional or sequence through its built-in widget, anything else
/// through Control<T>.
template <typename T>
void draw(T& _object)
{
    if constexpr (has_control<T>) {
        Control<T>{}.draw(_object);
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
            // field_label runs the annotation lookup in a constant context; a
            // reflection query in a runtime expression is not reliable.
            const std::string member_label = field_label<field>();

            if constexpr (is_widget_annotated<field>()) {
                field_row(member_label, [&] { draw_annotated<field>(_object.[:field:]); });
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
