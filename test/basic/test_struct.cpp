#include <gtest/gtest.h>

#include "scaffold/struct.h"

// Struct reflection: member count / name / has-member queries (compile-time),
// then two encoders built by walking the reflected member list (runtime, so
// round-trip tested with EXPECT).

// --- compile-time: the member queries ----------------------------------------

static_assert(impl_reflection_member_count() == 3,
              "MyStruct has three members");
static_assert(impl_reflection_member<0>() == "m_pub_i", "member 0 is m_pub_i");
static_assert(impl_reflection_member<2>() == "m_pub_s", "member 2 is m_pub_s");
static_assert(impl_reflection_has_member("m_pub_d"),
              "has_member finds m_pub_d");
static_assert(!impl_reflection_has_member("nope"),
              "has_member rejects an absent name");

// --- runtime: both encoders walk the same reflected list
// ----------------------

TEST(StructReflection, yaml_round_trip) {
    const MyStruct original = sample();
    MyStruct restored{};
    impl_reflection_load(restored, impl_reflection_dump(original));

    EXPECT_EQ(restored.m_pub_i, original.m_pub_i);
    EXPECT_EQ(restored.m_pub_d, original.m_pub_d);
    EXPECT_EQ(restored.m_pub_s, original.m_pub_s);
}

TEST(StructReflection, raw_byte_round_trip) {
    const MyStruct original = sample();
    MyStruct restored{};
    impl_reflection_deserialize(restored, impl_reflection_serialize(original));

    EXPECT_EQ(restored.m_pub_i, original.m_pub_i);
    EXPECT_EQ(restored.m_pub_d, original.m_pub_d);
    EXPECT_EQ(restored.m_pub_s, original.m_pub_s);
}
