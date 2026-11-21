#pragma once

#include "json_reflection.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace js {

enum class Style {
    /**
     * @brief Bare JSON Schema.
     */
    JsonSchema,
    /**
     * @brief OpenAI function tool with name and description.
     */
    OpenAiTool,
};

/**
 * @brief Bare JSON Schema for a C++ value, including scalar and container
 * types.
 */
template <typename T> consteval std::string param_schema();

namespace detail {

template <typename T> consteval std::size_t member_count() {
    return meta::nonstatic_data_members_of(^^T,
                                           meta::access_context::unchecked())
        .size();
}

template <typename T>
consteval std::array<meta::info, member_count<T>()> member_infos() {
    const auto members =
        meta::nonstatic_data_members_of(^^T, meta::access_context::unchecked());
    std::array<meta::info, member_count<T>()> out{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        out[i] = members[i];
    }
    return out;
}

template <typename T> consteval bool can_schema();

template <typename T, std::size_t... Is>
consteval bool can_schema_variant(std::index_sequence<Is...>) {
    return (can_schema<std::variant_alternative_t<Is, T>>() && ...);
}

template <typename T, std::size_t... Is>
consteval bool can_schema_members(std::index_sequence<Is...>) {
    constexpr auto infos = member_infos<T>();
    return (can_schema<
                std::remove_cvref_t<typename[:meta::type_of(infos[Is]):]>>() &&
            ...);
}

template <typename T> consteval bool can_schema() {
    using U = std::remove_cvref_t<T>;
    if constexpr (is_optional<U>::value) {
        return can_schema<typename U::value_type>();
    } else if constexpr (is_sequence<U>::value) {
        return can_schema<typename is_sequence<U>::value_type>();
    } else if constexpr (is_variant<U>::value) {
        return can_schema_variant<U>(
            std::make_index_sequence<std::variant_size_v<U>>());
    } else if constexpr (std::is_enum_v<U>) {
        return true;
    } else if constexpr (std::is_class_v<U> && !is_string_like_v<U>) {
        return can_schema_members<U>(
            std::make_index_sequence<member_count<U>()>());
    } else {
        return is_json_scalar_v<U>;
    }
}

template <typename T, std::size_t... Is>
consteval std::string object_schema_impl(std::index_sequence<Is...>) {
    constexpr auto infos = member_infos<T>();
    std::string properties;
    std::string required;
    ((void)([&] {
         using MT = std::remove_cvref_t<typename[:meta::type_of(infos[Is]):]>;
         const std::string name(meta::identifier_of(infos[Is]));
         if (!properties.empty()) {
             properties += ",";
         }
         properties += "\"" + name + "\":" +
                       with_description(param_schema<MT>(),
                                        text_of<^^js::desc, infos[Is]>());
         if constexpr (is_optional<MT>::value ||
                       !meta::has_default_member_initializer(infos[Is])) {
             if (!required.empty()) {
                 required += ",";
             }
             required += "\"" + name + "\"";
         }
     }()),
     ...);
    return "{\"type\":\"object\",\"properties\":{" + properties +
           "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

template <typename T, std::size_t... Is>
consteval std::string variant_schema_impl(std::index_sequence<Is...>);

}

template <typename T> consteval std::string param_schema() {
    if constexpr (detail::is_optional<T>::value) {
        return "{\"anyOf\":[" + param_schema<typename T::value_type>() +
               ",{\"type\":\"null\"}]}";
    } else if constexpr (detail::is_sequence<T>::value) {
        std::string out = "{\"type\":\"array\",\"items\":" +
                          param_schema<typename T::value_type>();
        if constexpr (detail::is_sequence<T>::fixed_size) {
            const std::string n =
                detail::number_to_json(detail::is_sequence<T>::extent);
            out += ",\"minItems\":" + n + ",\"maxItems\":" + n;
        }
        out += "}";
        return out;
    } else if constexpr (detail::is_variant<T>::value) {
        return detail::variant_schema_impl<T>(
            std::make_index_sequence<std::variant_size_v<T>>());
    } else if constexpr (std::is_enum_v<T>) {
        std::string values;
        for (std::meta::info e : std::meta::enumerators_of(^^T)) {
            if (!values.empty()) {
                values += ",";
            }
            values += "\"" + std::string(std::meta::identifier_of(e)) + "\"";
        }
        return "{\"type\":\"string\",\"enum\":[" + values + "]}";
    } else if constexpr (std::is_class_v<T> && !detail::is_string_like_v<T>) {
        return detail::object_schema_impl<T>(
            std::make_index_sequence<detail::member_count<T>()>());
    } else {
        static_assert(
            detail::is_json_scalar_v<T>,
            "param_schema: unsupported type for JSON Schema generation "
            "(only bool, integers, floats, strings, options, sequences, enums, "
            "variants and reflected classes are supported)");
        return std::string("{\"type\":\"") + detail::json_type_name(^^T) +
               "\"}";
    }
}

namespace detail {

template <typename T, std::size_t... Is>
consteval std::string variant_schema_impl(std::index_sequence<Is...>) {
    std::string out = "{\"anyOf\":[";
    ((void)([&] {
         out += std::string(Is == 0 ? "" : ",");
         out += param_schema<std::variant_alternative_t<Is, T>>();
     }()),
     ...);
    out += "]}";
    return out;
}

template <meta::info Fn, std::size_t I>
using param_type_at =
    std::remove_cvref_t<typename[:meta::type_of(param_at<Fn, I>()):]>;

template <meta::info Fn, std::size_t... Is>
consteval std::string param_object_schema_impl(std::index_sequence<Is...>) {
    std::string properties;
    std::string required;
    ((void)([&] {
         using PT = param_type_at<Fn, Is>;
         const std::string name(meta::identifier_of(param_at<Fn, Is>()));
         if (!properties.empty()) {
             properties += ",";
         }
         properties +=
             "\"" + name + "\":" +
             with_description(param_schema<PT>(), param_description<Fn, Is>());
         if (!required.empty()) {
             required += ",";
         }
         required += "\"" + name + "\"";
     }()),
     ...);
    return "{\"type\":\"object\",\"properties\":{" + properties +
           "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

template <meta::info Fn> consteval std::string param_object_schema() {
    return param_object_schema_impl<Fn>(
        std::make_index_sequence<param_count<Fn>()>());
}

template <meta::info Fn> consteval std::string description_of() {
    return text_of<^^js::doc, Fn>();
}

constexpr std::string build_openai_tool(std::string_view _name,
                                        std::string_view _description,
                                        std::string_view _parameters) {
    std::string out = "{\"type\":\"function\",\"function\":{\"name\":\"";
    out += _name;
    out += "\",\"description\":\"" + json_escape(_description) +
           "\",\"parameters\":";
    out += _parameters;
    out += "}}";
    return out;
}

}

/**
 * @brief Generates the schema for the parameters of a function.
 * @details The OpenAI tool form uses the function description from js::doc.
 * @tparam Fn Reflection of the function to describe.
 * @tparam S Output format.
 * @return JSON text in static storage.
 */
template <std::meta::info Fn, Style S = Style::JsonSchema>
consteval std::string_view function_schema() {
    const auto parameters = detail::param_object_schema<Fn>();
    if constexpr (S == Style::OpenAiTool) {
        return detail::static_text(detail::build_openai_tool(
            std::meta::identifier_of(Fn), detail::description_of<Fn>(),
            parameters));
    } else {
        return detail::static_text(parameters);
    }
}

/**
 * @brief Generates the schema for the fields of a struct.
 * @details The OpenAI tool form uses the type description from js::doc.
 * @tparam T The struct to describe.
 * @tparam S Output format.
 * @return JSON text in static storage.
 */
template <typename T, Style S = Style::JsonSchema>
consteval std::string_view tool_schema() {
    const auto fields = detail::object_schema_impl<T>(
        std::make_index_sequence<detail::member_count<T>()>());
    if constexpr (S == Style::OpenAiTool) {
        return detail::static_text(
            detail::build_openai_tool(std::meta::identifier_of(^^T),
                                      detail::description_of<^^T>(), fields));
    } else {
        return detail::static_text(fields);
    }
}

}
