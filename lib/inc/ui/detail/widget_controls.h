#pragma once

#include "../../detail/reflection.h"
#include "widget_annotations.h"

#include <array>
#include <cstddef>
#include <memory>
#include <meta>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <imgui.h>
#include <imgui_stdlib.h>

namespace ui {

/**
 * @brief Specialize Control<T> to draw an application type as a leaf widget.
 * @details The specialization must be default-constructible and provide
 * draw(T&).
 */
template <typename T> struct Control {};

/**
 * @brief Selects an ImGui function and supplies its named parameters.
 * Members the selected function does not take have no effect.
 */
struct control {
    std::meta::info fn;
    float v_speed = 1.0f;
    double v_min = 0.0;
    double v_max = 0.0;
    double step = 1.0;
    double step_fast = 100.0;
    double p_min = 0.0;
    double p_max = 0.0;
    double p_step = 0.0;
    double p_step_fast = 0.0;
};

/**
 * @brief Selects the ImGui::InputText overload for std::string.
 */
consteval std::meta::info string_input_text() {
    using Signature = bool(const char *, std::string *, ImGuiInputTextFlags,
                           ImGuiInputTextCallback, void *);
    return std::meta::reflect_function(
        static_cast<Signature &>(ImGui::InputText));
}

/**
 * @brief Selects the ImGui::InputText overload for a char buffer.
 */
consteval std::meta::info charptr_input_text() {
    using Signature = bool(const char *, char *, std::size_t,
                           ImGuiInputTextFlags, ImGuiInputTextCallback, void *);
    return std::meta::reflect_function(
        static_cast<Signature &>(ImGui::InputText));
}

/**
 * @brief Specialize default_control<T> with a static constexpr control value.
 */
template <typename T> struct default_control {};
template <> struct default_control<bool> {
    static constexpr control value{.fn = ^^ImGui::Checkbox};
};
template <typename T>
    requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
struct default_control<T> {
    static constexpr control value{.fn = ^^ImGui::DragScalar};
};
template <> struct default_control<std::string> {
    static constexpr control value{.fn = string_input_text()};
};
template <std::size_t N> struct default_control<char[N]> {
    static constexpr control value{.fn = charptr_input_text()};
};

/**
 * @brief Draw with the control annotation on a field.
 */
template <std::meta::info Field> struct field_control {};

/**
 * @brief Draw with the default_control for a type.
 */
template <typename T> struct type_control {};

namespace detail {

using cppreflect::detail::param_at;

template <typename T>
concept has_control = requires(T &_value) { Control<T>{}.draw(_value); };

template <typename T>
concept has_default_control = requires { default_control<T>::value; };

template <typename T> constexpr ImGuiDataType imgui_data_type() {
    if constexpr (std::is_same_v<T, float>) {
        return ImGuiDataType_Float;
    } else if constexpr (std::is_same_v<T, double>) {
        return ImGuiDataType_Double;
    } else if constexpr (std::is_signed_v<T>) {
        if constexpr (sizeof(T) == 1)
            return ImGuiDataType_S8;
        else if constexpr (sizeof(T) == 2)
            return ImGuiDataType_S16;
        else if constexpr (sizeof(T) == 4)
            return ImGuiDataType_S32;
        else
            return ImGuiDataType_S64;
    } else {
        if constexpr (sizeof(T) == 1)
            return ImGuiDataType_U8;
        else if constexpr (sizeof(T) == 2)
            return ImGuiDataType_U16;
        else if constexpr (sizeof(T) == 4)
            return ImGuiDataType_U32;
        else
            return ImGuiDataType_U64;
    }
}

inline constexpr std::array control_functions{
    ^^ImGui::SliderInt, ^^ImGui::SliderFloat, ^^ImGui::SliderScalar,
    ^^ImGui::DragInt,   ^^ImGui::DragFloat,   ^^ImGui::DragScalar,
    ^^ImGui::InputInt,  ^^ImGui::InputDouble, ^^ImGui::InputScalar,
    ^^ImGui::Checkbox,  string_input_text(),  charptr_input_text(),
};

template <std::meta::info Fn> consteval bool is_control_function() {
    bool found = false;
    template for (constexpr std::meta::info f : control_functions) {
        if constexpr (Fn == f) {
            found = true;
        }
    }
    return found;
}

consteval bool control_members_are_parameters() {
    for (std::meta::info member : std::meta::nonstatic_data_members_of(
             ^^control, std::meta::access_context::unchecked())) {
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
              "every ui::control member must name a parameter of a supported "
              "ImGui function");

template <typename Source> struct control_traits;
template <std::meta::info Field> struct control_traits<field_control<Field>> {
    static consteval std::meta::info function() {
        return annotation_value<^^control, Field>().fn;
    }
    template <std::meta::info Member> static consteval auto member() {
        return annotation_value<^^control, Field>().[:Member:];
    }
};
template <typename T> struct control_traits<type_control<T>> {
    static consteval std::meta::info function() {
        return default_control<T>::value.fn;
    }
    template <std::meta::info Member> static consteval auto member() {
        return default_control<T>::value.[:Member:];
    }
};

template <std::meta::info Fn, std::size_t I>
using param_type_at =
    std::remove_cvref_t<typename[:std::meta::type_of(param_at<Fn, I>()):]>;

template <std::meta::info A, std::meta::info P>
consteval std::meta::info control_argument_of() {
    for (std::meta::info m : std::meta::nonstatic_data_members_of(
             A, std::meta::access_context::unchecked())) {
        if (std::meta::identifier_of(m) == std::meta::identifier_of(P)) {
            return m;
        }
    }
    return {};
}

template <typename Source, std::meta::info Fn, std::size_t I, typename T,
          typename Storage>
void bind_argument(Storage &_storage, auto &_args, const char *_label,
                   T &_value) {
    using PT = param_type_at<Fn, I>;
    constexpr std::string_view name = cppreflect::detail::static_text(
        std::meta::identifier_of(param_at<Fn, I>()));
    constexpr std::meta::info member =
        control_argument_of<^^control, param_at<Fn, I>()>();
    if constexpr (name == "label") {
        std::get<I>(_args) = _label;
    } else if constexpr (name == "v" || name == "p_data" || name == "str") {
        static_assert(
            std::is_same_v<std::remove_cv_t<std::remove_pointer_t<PT>>, T> ||
                std::is_void_v<std::remove_cv_t<std::remove_pointer_t<PT>>>,
            "the controlled value's type must be the one this function takes");
        std::get<I>(_args) = static_cast<PT>(std::addressof(_value));
    } else if constexpr (name == "buf") {
        static_assert(std::is_array_v<T>, "a buf parameter wants a char array");
        std::get<I>(_args) = static_cast<PT>(_value);
    } else if constexpr (name == "buf_size") {
        static_assert(std::is_array_v<T>,
                      "a buf_size parameter comes from a char array's extent");
        std::get<I>(_args) = static_cast<PT>(std::extent_v<T>);
    } else if constexpr (name == "data_type") {
        std::get<I>(_args) = imgui_data_type<T>();
    } else if constexpr (member != std::meta::info{}) {
        constexpr auto raw = control_traits<Source>::template member<member>();
        if constexpr (std::is_pointer_v<PT>) {
            static_assert(
                std::is_void_v<std::remove_cv_t<std::remove_pointer_t<PT>>>,
                "a bound pointer parameter must be a void* to the value's "
                "type");
            _storage[I] = static_cast<T>(raw);
            std::get<I>(_args) = static_cast<PT>(std::addressof(_storage[I]));
        } else {
            std::get<I>(_args) = static_cast<PT>(raw);
        }
    }
}

template <typename Source, typename T, std::size_t... Is>
void invoke_impl(const char *_label, T &_value, std::index_sequence<Is...>) {
    constexpr std::meta::info fn = control_traits<Source>::function();
    std::tuple<param_type_at<fn, Is>...> args{};
    std::array<T, sizeof...(Is)> storage{};
    (bind_argument<Source, fn, Is>(storage, args, _label, _value), ...);
    std::apply(&[:fn:], args);
}

template <std::meta::info Field> consteval bool is_widget_annotated() {
    return has_annotation<^^control, Field>();
}

}

/**
 * @brief Draws a value using a field or type control.
 * @tparam Source The field_control or type_control selecting the widget.
 * @param _label The ImGui label for the widget.
 * @param[in,out] _value The value to display and edit.
 */
template <typename Source, typename T>
void draw_control(const char *_label, T &_value) {
    constexpr std::meta::info fn = detail::control_traits<Source>::function();
    static_assert(detail::is_control_function<fn>(),
                  "ui::control: fn must be a supported function");
    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    detail::invoke_impl<Source>(
        _label, _value,
        std::make_index_sequence<cppreflect::detail::param_count<fn>()>{});
    ImGui::PopID();
}

}
