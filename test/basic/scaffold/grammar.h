#pragma once

// Scaffolding for test_grammar.cpp: the type being reflected and the
// queries over it, plus a namespace used to show is_namespace. Included by
// exactly one test translation unit, so it keeps its file-scoped anonymous
// namespace.

#include <cstddef>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>

namespace {

struct PodStruct {
    int m_mem_int;
    double m_mem_real;
    std::string m_mem_str;

    PodStruct operator+([[maybe_unused]] const PodStruct &_other) const {
        return {};
    }
};

consteval std::size_t pod_member_count() {
    return std::define_static_array(
               std::meta::nonstatic_data_members_of(
                   ^^PodStruct, std::meta::access_context::unchecked()))
        .size();
}

consteval bool all_members_are_nonstatic() {
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^PodStruct, std::meta::access_context::unchecked()))) {
        if (!std::meta::is_nonstatic_data_member(member)) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace MyNameSpace {
namespace Woops {}
} // namespace MyNameSpace
