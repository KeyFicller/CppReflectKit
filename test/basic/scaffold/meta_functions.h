#pragma once

// Scaffolding for test_meta_functions.cpp: a class, enum, union and class
// template to query, plus the consteval helpers wrapping individual queries.
// Included by exactly one test translation unit, so it keeps its file-scoped
// anonymous namespace.

#include <cstddef>
#include <meta>
#include <string>
#include <string_view>

namespace MyNameSpace {

struct MyStructBase {};

struct MyStruct : public MyStructBase {
public:
    int m_pub_i = 1;
    static int s_pub_i;

protected:
    double m_pri_d = 2.0;

private:
    std::string m_pro_s = "Hello";
};

int MyStruct::s_pub_i = 3;

} // namespace MyNameSpace

enum class MyEnum {
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};

union MyUnion {
    int m_i;
    void *m_p;
};

template <typename T, typename R = double> class MyClassTemplate {
public:
    T m_t;
};

namespace {

consteval std::string_view my_struct_identifier() {
    return std::meta::identifier_of(^^MyNameSpace::MyStruct);
}

consteval std::size_t nonstatic_member_count() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(
                                        ^^MyNameSpace::MyStruct,
                                        std::meta::access_context::unchecked()))
        .size();
}

consteval std::size_t static_member_count() {
    return std::define_static_array(std::meta::static_data_members_of(
                                        ^^MyNameSpace::MyStruct,
                                        std::meta::access_context::unchecked()))
        .size();
}

consteval std::size_t base_count() {
    return std::define_static_array(
               std::meta::bases_of(^^MyNameSpace::MyStruct,
                                   std::meta::access_context::unchecked()))
        .size();
}

consteval std::size_t enumerator_count() {
    return std::define_static_array(std::meta::enumerators_of(^^MyEnum)).size();
}

consteval bool first_template_argument_is_int() {
    return std::define_static_array(std::meta::template_arguments_of(
               ^^MyClassTemplate<int>))[0] == ^^int;
}

} // namespace
