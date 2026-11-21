#pragma once

#include "../../detail/reflection.h"
#include "../../detail/type_traits.h"

#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace js {

namespace detail {
struct StringTag;
}

/**
 * @brief Structural string storage for annotations, including the terminating
 * NUL.
 */
template <std::size_t N>
using str = cppreflect::detail::fixed_string<N, detail::StringTag>;

/**
 * @brief Supplies the reflected description for a function or type.
 * @details Corresponds to the brief summary of a documented declaration.
 */
template <std::size_t N = 1> struct doc {
    str<N> text;
};

/**
 * @brief Supplies the description of a reflected field in JSON Schema.
 * @details Corresponds to the brief summary in a field's API documentation.
 */
template <std::size_t N = 1> struct desc {
    str<N> text;
};

namespace detail {

template <typename... Ts> struct doc_list;
template <> struct doc_list<> {};
template <typename T, typename... Ts> struct doc_list<T, Ts...> {
    T head;
    doc_list<Ts...> tail;

    constexpr doc_list() = default;
    constexpr doc_list(T first, Ts... rest) : head(first), tail(rest...) {}
};

}

/**
 * @brief Attaches reflected parameter descriptions to a function.
 * @param descriptions Description strings in declaration order.
 * @see js::doc
 */
consteval auto param_docs(auto... descriptions) {
    return detail::doc_list<decltype(descriptions)...>{descriptions...};
}

namespace detail {

namespace meta = std::meta;
using cppreflect::detail::find_annotation;
using cppreflect::detail::is_optional;
using cppreflect::detail::is_sequence;
using cppreflect::detail::param_at;
using cppreflect::detail::param_count;
using cppreflect::detail::static_text;

template <typename T> struct is_variant : std::false_type {};
template <typename... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_like_v =
    std::is_same_v<std::remove_cv_t<T>, std::string> ||
    std::is_same_v<std::remove_cv_t<T>, std::string_view>;

template <typename T>
inline constexpr bool is_json_scalar_v =
    std::is_same_v<std::remove_cv_t<T>, bool> || std::is_integral_v<T> ||
    std::is_floating_point_v<T> || is_string_like_v<T>;

constexpr std::string json_escape(std::string_view _s) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (char c : _s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (u < 0x20) {
                out += "\\u00";
                out += kHex[(u >> 4) & 0xF];
                out += kHex[u & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

template <typename T> constexpr std::string number_to_json(T _value) {
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof buf, _value);
    return std::string(buf, result.ptr);
}

consteval const char *json_type_name(meta::info _t) {
    _t = meta::dealias(_t);
    if (meta::is_same_type(_t, ^^bool)) {
        return "boolean";
    }
    if (meta::is_integral_type(_t)) {
        return "integer";
    }
    if (meta::is_floating_point_type(_t)) {
        return "number";
    }
    if (meta::is_same_type(_t, ^^std::string) ||
        meta::is_same_type(_t, ^^std::string_view)) {
        return "string";
    }
    return "string";
}

template <meta::info Tmpl, meta::info E> consteval std::string text_of() {
    if constexpr (find_annotation<Tmpl>(E) == meta::info{}) {
        return {};
    } else {
        constexpr auto cfg =
            meta::extract<typename[:meta::type_of(find_annotation<Tmpl>(E)):]>(
                find_annotation<Tmpl>(E));
        return std::string(std::string_view(cfg.text.data));
    }
}

template <meta::info Fn> consteval std::size_t param_doc_count() {
    if (find_annotation<^^doc_list>(Fn) == meta::info{}) {
        return 0;
    }
    return meta::template_arguments_of(meta::remove_const(meta::type_of(
                                           find_annotation<^^doc_list>(Fn))))
        .size();
}

template <std::size_t I, typename List> consteval auto nth_doc(List _list) {
    if constexpr (I == 0) {
        return _list.head;
    } else {
        return nth_doc<I - 1>(_list.tail);
    }
}

template <meta::info Fn, std::size_t I>
consteval std::string param_description() {
    if constexpr (find_annotation<^^doc_list>(Fn) == meta::info{}) {
        return {};
    } else if constexpr (I < param_doc_count<Fn>()) {
        constexpr auto list = meta::extract<
            typename[:meta::type_of(find_annotation<^^doc_list>(Fn)):]>(
            find_annotation<^^doc_list>(Fn));
        return std::string(std::string_view(nth_doc<I>(list).data));
    } else {
        return {};
    }
}

consteval std::string with_description(std::string _schema,
                                       std::string_view _desc) {
    if (_desc.empty()) {
        return _schema;
    }
    return _schema.substr(0, _schema.size() - 1) + ",\"description\":\"" +
           json_escape(_desc) + "\"}";
}

}
}
