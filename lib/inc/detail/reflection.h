#pragma once

#include <algorithm>
#include <cstddef>
#include <meta>
#include <string_view>

namespace cppreflect::detail {

template <std::size_t N, typename Tag> struct fixed_string {
    char data[N]{};

    constexpr fixed_string() = default;
    constexpr fixed_string(const char (&text)[N]) {
        std::copy_n(text, N, data);
    }
};

consteval std::string_view static_text(std::string_view text) {
    return {std::define_static_string(text), text.size()};
}

template <std::meta::info Annotation>
consteval std::meta::info find_annotation(std::meta::info entity) {
    for (const auto annotation : std::meta::annotations_of(entity)) {
        const auto type =
            std::meta::remove_const(std::meta::type_of(annotation));
        if constexpr (std::meta::is_type(Annotation)) {
            if (std::meta::is_same_type(type, Annotation)) {
                return annotation;
            }
        } else {
            if (std::meta::has_template_arguments(type) &&
                std::meta::template_of(type) == Annotation) {
                return annotation;
            }
        }
    }
    return {};
}

template <std::meta::info Annotation, std::meta::info Entity>
consteval bool has_annotation() {
    return find_annotation<Annotation>(Entity) != std::meta::info{};
}

template <std::meta::info Fn> consteval std::size_t param_count() {
    return std::meta::parameters_of(Fn).size();
}

template <std::meta::info Fn, std::size_t I>
consteval std::meta::info param_at() {
    return std::meta::parameters_of(Fn)[I];
}

}
