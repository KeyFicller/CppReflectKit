#pragma once

// Scaffolding for test_enumeration.cpp: the enums and the enumerator-walking
// helpers (name <-> value, enumerator list, bitmask rendering). Included by
// exactly one test translation unit.

#include <array>
#include <concepts>
#include <cstdint>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

enum class WeekDay {
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};

enum class BitEnum : uint32_t {
    kRead = 0x01,
    kWrite = 0x02,
    kXn = 0x04
};

const char* impl_classic_(WeekDay _week_day)
{
    switch (_week_day) {
        case WeekDay::Monday:
            return "Monday";
        case WeekDay::Tuesday:
            return "Tuesday";
        case WeekDay::Wednesday:
            return "Wednesday";
        case WeekDay::Thursday:
            return "Thursday";
        case WeekDay::Friday:
            return "Friday";
        case WeekDay::Saturday:
            return "Saturday";
        case WeekDay::Sunday:
            return "Sunday";
        default:
            return "Unknown";
    }
}

template <typename E>
requires std::is_enum_v<E>
const char* impl_reflection_(E _value)
{
    template for (constexpr std::meta::info e : std::define_static_array(
        std::meta::enumerators_of(^^E))
    )
    {
        if (_value == [:e:]) {
            return std::meta::display_string_of(e).data();
        }
    }
    return "Unknown";
}

template <typename E>
requires std::is_enum_v<E>
constexpr std::optional<E> impl_reflection_(std::string_view _str)
{
    template for (constexpr std::meta::info e : std::define_static_array(
        std::meta::enumerators_of(^^E))
    )
    {
        if (_str == std::meta::identifier_of(e)) {
            return [:e:];
        }
    }
    return std::nullopt;
}

template <typename E>
requires std::is_enum_v<E>
consteval auto impl_reflection_enum_list_val()
{
    constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<E, enumerators.size()> values;
    std::size_t index = 0;
    template for (constexpr std::meta::info e : enumerators)
    {
        values[index++] = [:e:];
    }
    return values;
}

template <typename E>
requires std::is_enum_v<E>
consteval auto impl_reflection_enum_list_str()
{
    constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::string_view, enumerators.size()> values;
    std::size_t index = 0;
    template for (constexpr std::meta::info e : enumerators)
    {
        values[index++] = std::meta::identifier_of(e);
    }
    return values;
}

template <typename E>
requires std::is_enum_v<E>
std::string impl_reflection_enum_bits_str(E _value)
{
    std::string str;
    auto raw = static_cast<std::underlying_type_t<E>>(_value);

    constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    template for (constexpr std::meta::info e : enumerators)
    {
        auto bit = static_cast<std::underlying_type_t<E>>([:e:]);
        if (raw & bit) {
            if (!str.empty()) {
                str += " | ";
            }
            str += std::meta::identifier_of(e);
        }
    }
    return str;
}
