#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <vector>

namespace cppreflect::detail {

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T> struct is_sequence : std::false_type {};
template <typename T, typename Allocator>
struct is_sequence<std::vector<T, Allocator>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = false;
};
template <typename T, std::size_t N>
struct is_sequence<std::array<T, N>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = true;
    static constexpr std::size_t extent = N;
};

}
