#include <gtest/gtest.h>

#include "scaffold/enumeration.h"

// Enumeration reflection: name <-> value, the enumerator list, and bitmask
// rendering, all driven by enumerators_of. The compile-time results are
// static_assert; the two functions that build a std::string at runtime are TEST.

// --- compile-time: the enumerator list, and name -> value ---------------------

static_assert(impl_reflection_enum_list_val<WeekDay>().size() == 7,
              "the value array holds seven enumerators");
static_assert(impl_reflection_enum_list_str<WeekDay>()[0] == "Monday",
              "the name array starts at Monday");
static_assert(impl_reflection_enum_list_str<WeekDay>()[6] == "Sunday",
              "the name array ends at Sunday");

static_assert(impl_reflection_<WeekDay>(std::string_view("Thursday")) == WeekDay::Thursday,
              "a name parses back to its enumerator");
static_assert(impl_reflection_<WeekDay>(std::string_view("Nonsense")) == std::nullopt,
              "an unknown name yields nullopt");

// --- runtime: the string builders --------------------------------------------

TEST(EnumerationReflection, value_to_name)
{
    // The reflected version must agree with the hand-written switch.
    EXPECT_STREQ(impl_reflection_(WeekDay::Monday), impl_classic_(WeekDay::Monday));
    EXPECT_STREQ(impl_reflection_(WeekDay::Sunday), "Sunday");
}

TEST(EnumerationReflection, bitmask_renders_enum_names)
{
    const auto bits = static_cast<BitEnum>(static_cast<int>(BitEnum::kRead) |
                                          static_cast<int>(BitEnum::kWrite));
    EXPECT_EQ(impl_reflection_enum_bits_str(bits), "kRead | kWrite");
}
