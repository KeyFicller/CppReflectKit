#include "scaffold/meta_functions.h"

// A catalogue of the query functions. Every result is compile-time, so the whole
// catalogue is one static_assert per query.

// Name queries.
static_assert(my_struct_identifier() == "MyStruct", "identifier_of(MyStruct)");

// Type queries.
static_assert(std::meta::is_type(^^MyNameSpace::MyStruct), "MyStruct is a type");
static_assert(!std::meta::is_namespace(^^MyNameSpace::MyStruct), "MyStruct is not a namespace");
static_assert(std::meta::is_namespace(^^MyNameSpace), "MyNameSpace is a namespace");

// Member queries.
static_assert(nonstatic_member_count() == 3, "MyStruct has three nonstatic members");
static_assert(static_member_count() == 1, "MyStruct has one static member");
static_assert(base_count() == 1, "MyStruct has one base");
static_assert(enumerator_count() == 7, "MyEnum has seven enumerators");

// Classification queries.
static_assert(std::meta::is_class_type(^^MyNameSpace::MyStruct), "is_class_type(MyStruct)");
static_assert(std::meta::is_enum_type(^^MyEnum), "is_enum_type(MyEnum)");
static_assert(std::meta::is_union_type(^^MyUnion), "is_union_type(MyUnion)");
static_assert(std::meta::is_nonstatic_data_member(^^MyNameSpace::MyStruct::m_pub_i),
              "is_nonstatic_data_member(m_pub_i)");
static_assert(std::meta::is_static_member(^^MyNameSpace::MyStruct::s_pub_i),
              "is_static_member(s_pub_i)");
static_assert(std::meta::is_class_template(^^MyClassTemplate), "is_class_template(MyClassTemplate)");
static_assert(std::meta::is_template(^^MyClassTemplate), "is_template(MyClassTemplate)");
static_assert(first_template_argument_is_int(),
              "template_arguments_of(MyClassTemplate<int>)[0] is int");
