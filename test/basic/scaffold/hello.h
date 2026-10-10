#pragma once

// Scaffolding for test_hello.cpp: the struct and enum being reflected,
// plus the consteval helpers that query them. Included by exactly one test
// translation unit, so it keeps its file-scoped anonymous namespace.

#include <cstddef>
#include <meta>
#include <string>
#include <string_view>

namespace {

struct PodStruct {
    int m_mem_int;
    double m_mem_real;
    std::string m_mem_str;
};

enum class WeekDay {
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};

consteval std::size_t pod_member_count()
{
    return std::define_static_array(std::meta::nonstatic_data_members_of(
                                        ^^PodStruct, std::meta::access_context::unchecked()))
        .size();
}

consteval std::string_view pod_member(std::size_t _index)
{
    return std::meta::identifier_of(
        std::define_static_array(std::meta::nonstatic_data_members_of(
                                     ^^PodStruct, std::meta::access_context::unchecked()))[_index]);
}

consteval std::size_t weekday_count()
{
    return std::define_static_array(std::meta::enumerators_of(^^WeekDay)).size();
}

consteval std::string_view weekday(std::size_t _index)
{
    return std::meta::identifier_of(
        std::define_static_array(std::meta::enumerators_of(^^WeekDay))[_index]);
}

} // namespace
