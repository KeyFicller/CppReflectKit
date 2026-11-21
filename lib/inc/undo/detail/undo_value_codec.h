#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../../detail/type_traits.h"

namespace undo::detail {

using cppreflect::detail::is_optional;
using cppreflect::detail::is_sequence;

template <typename T>
inline constexpr bool is_string_v =
    std::is_same_v<std::remove_cv_t<T>, std::string>;

template <typename T> void encode(std::string &_out, const T &_value) {
    if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
        _out.push_back(static_cast<char>(_value ? 1 : 0));
    } else if constexpr (std::is_enum_v<T>) {
        encode(_out, static_cast<std::underlying_type_t<T>>(_value));
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        _out.append(reinterpret_cast<const char *>(&_value), sizeof(T));
    } else if constexpr (is_string_v<T>) {
        const std::uint32_t count = static_cast<std::uint32_t>(_value.size());
        _out.append(reinterpret_cast<const char *>(&count), sizeof(count));
        _out.append(_value.data(), count);
    } else if constexpr (is_optional<T>::value) {
        _out.push_back(static_cast<char>(_value.has_value() ? 1 : 0));
        if (_value) {
            encode(_out, *_value);
        }
    } else if constexpr (is_sequence<T>::value) {
        if constexpr (!is_sequence<T>::fixed_size) {
            const auto count = static_cast<std::uint32_t>(_value.size());
            _out.append(reinterpret_cast<const char *>(&count), sizeof(count));
        }
        for (const auto &element : _value) {
            encode(_out, element);
        }
    } else if constexpr (std::is_class_v<T>) {
        constexpr auto members =
            std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info member : members) {
            encode(_out, _value.[:member:]);
        }
    } else {
        static_assert(
            !sizeof(T),
            "undo: unsupported field type (expected a scalar, string, enum, "
            "optional, vector, array or reflected struct)");
    }
}

template <typename T>
void decode(std::string_view _in, std::size_t &_offset, T &_out) {
    if constexpr (std::is_same_v<std::remove_cv_t<T>, bool>) {
        _out = _in[_offset++] != 0;
    } else if constexpr (std::is_enum_v<T>) {
        std::underlying_type_t<T> raw{};
        decode(_in, _offset, raw);
        _out = static_cast<T>(raw);
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        std::memcpy(&_out, _in.data() + _offset, sizeof(T));
        _offset += sizeof(T);
    } else if constexpr (is_string_v<T>) {
        std::uint32_t count{};
        std::memcpy(&count, _in.data() + _offset, sizeof(count));
        _offset += sizeof(count);
        _out.assign(_in.data() + _offset, count);
        _offset += count;
    } else if constexpr (is_optional<T>::value) {
        const bool present = _in[_offset++] != 0;
        if (present) {
            typename T::value_type value{};
            decode(_in, _offset, value);
            _out = std::move(value);
        } else {
            _out.reset();
        }
    } else if constexpr (is_sequence<T>::value) {
        if constexpr (is_sequence<T>::fixed_size) {
            for (auto &element : _out) {
                decode(_in, _offset, element);
            }
        } else {
            std::uint32_t count{};
            std::memcpy(&count, _in.data() + _offset, sizeof(count));
            _offset += sizeof(count);
            _out.clear();
            _out.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                typename T::value_type element{};
                decode(_in, _offset, element);
                _out.push_back(std::move(element));
            }
        }
    } else if constexpr (std::is_class_v<T>) {
        constexpr auto members =
            std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info member : members) {
            decode(_in, _offset, _out.[:member:]);
        }
    } else {
        static_assert(!sizeof(T), "undo: unsupported field type");
    }
}

}
