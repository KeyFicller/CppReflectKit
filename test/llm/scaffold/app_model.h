#pragma once

// The example domain types. Their JSON Schemas are derived by reflection, so they
// only carry js::doc / js::desc annotations. The tools, prompts and resources
// that use them are member functions of app::CppReflectServer (see app_server.h).
//
// Model registration (CPP_REFLECT_MODEL) deliberately stays in
// test_json_schema.cpp so exactly one translation unit runs those static
// initializers.

#include "llm/reflection_json.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace app {

enum class Unit {
    Celsius,
    Fahrenheit
};

struct [[= js::doc{ .text = js::str("A numeric range to match.") } ]] Range {
    [[= js::desc{ .text = js::str("Inclusive lower bound.") } ]] int min = 0;
    [[= js::desc{ .text = js::str("Optional inclusive upper bound.") } ]] std::optional<int> max;
};

struct [[= js::doc{ .text = js::str("One search clause.") } ]] Query {
    [[= js::desc{ .text = js::str("The raw query text.") } ]] std::string text;
    [[= js::desc{ .text = js::str("Temperature unit for numeric filters.") } ]] Unit unit =
        Unit::Celsius;
    [[= js::desc{ .text = js::str("Numeric ranges to match.") } ]] std::vector<Range> ranges;
    [[= js::desc{ .text = js::str("Allow approximate matching.") } ]] bool fuzzy = false;
    [[= js::desc{ .text = js::str("Relative weight of this clause.") } ]] std::optional<double> weight;
};

struct [[= js::doc{ .text = js::str("A structured search request.") } ]] SearchRequest {
    Query query;
    [[= js::desc{ .text = js::str("Number of hits requested per shard.") } ]] std::array<int, 3> top_k{};
    bool debug = false;
};

} // namespace app
