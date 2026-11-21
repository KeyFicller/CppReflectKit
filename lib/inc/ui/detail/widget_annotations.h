#pragma once

#include "../../detail/reflection.h"

#include <cstddef>
#include <meta>
#include <string>

namespace ui {

namespace detail {
struct StringTag;
}

/**
 * @brief Structural text for use in a label annotation.
 */
template <std::size_t N>
using str = cppreflect::detail::fixed_string<N, detail::StringTag>;

/**
 * @brief Replaces the reflected member name in the form.
 */
template <std::size_t N = 1> struct label {
    str<N> text;
};

/**
 * @brief Omits the annotated field from the form.
 */
struct hidden {};

namespace detail {

using cppreflect::detail::has_annotation;

template <std::meta::info A, std::meta::info E>
consteval auto annotation_value() {
    constexpr auto annotation = cppreflect::detail::find_annotation<A>(E);
    return std::meta::extract<typename[:std::meta::type_of(annotation):]>(
        annotation);
}

template <std::meta::info E> consteval std::string annotation_label() {
    if constexpr (has_annotation<^^label, E>()) {
        constexpr auto cfg = annotation_value<^^label, E>();
        return std::string(cfg.text.data);
    } else {
        return {};
    }
}

template <std::meta::info Field> consteval std::string field_label() {
    if constexpr (has_annotation<^^label, Field>()) {
        return annotation_label<Field>();
    } else {
        constexpr auto name = std::meta::display_string_of(Field);
        return std::string(name.data(), name.size());
    }
}

template <std::meta::info Field> consteval bool is_hidden() {
    return has_annotation<^^hidden, Field>();
}

}
}
