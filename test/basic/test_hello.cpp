#include "scaffold/hello.h"

// Reflect a struct's members and an enum's enumerators. Every expectation here
// is known at compile time, so it is a static_assert: if the reflection yields
// something else the file does not compile, which is the test.

static_assert(pod_member_count() == 3, "PodStruct reflects three members");
static_assert(pod_member(0) == "m_mem_int", "member 0 is m_mem_int");
static_assert(pod_member(1) == "m_mem_real", "member 1 is m_mem_real");
static_assert(pod_member(2) == "m_mem_str", "member 2 is m_mem_str");

static_assert(weekday_count() == 7, "WeekDay reflects seven enumerators");
static_assert(weekday(0) == "Monday", "the first enumerator is Monday");
static_assert(weekday(6) == "Sunday", "the last enumerator is Sunday");
