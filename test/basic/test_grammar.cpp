#include "scaffold/grammar.h"

// The reflection operators (^^, [: :], ==) and the queries over them. Every
// value here is compile-time, so each expectation is a static_assert.

// Reflection operators.
static_assert(^^int == ^^int, "^^int == ^^int");
static_assert(!(^^int == ^^double), "^^int != ^^double");
static_assert(std::meta::has_identifier(^^PodStruct),
              "PodStruct has an identifier");
// An operator has a display string but no identifier: "operator+" is not an
// identifier. This is exactly the case has_identifier distinguishes.
static_assert(!std::meta::has_identifier(^^PodStruct::operator+),
              "an operator has no identifier");
static_assert(std::meta::is_namespace(^^MyNameSpace::Woops),
              "Woops is a namespace");
static_assert(std::meta::is_type(^^int), "int is a type");

// Splice operator.
static_assert(std::is_same_v<typename[:^^int:], int>,
              "splicing ^^int yields int");

// template for visits exactly the reflected nonstatic data members.
static_assert(pod_member_count() == 3, "PodStruct reflects three members");
static_assert(all_members_are_nonstatic(),
              "template for visits only nonstatic data members");
