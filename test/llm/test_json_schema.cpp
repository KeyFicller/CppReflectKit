#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "llm/reflection_json.h"
#include "scaffold/app_server.h"

// JSON Schema generated from C++26 static reflection, validated with a real
// JSON parser (nlohmann/json). The schemas are consteval strings, but their
// content is only meaningful once parsed, so these are runtime EXPECT checks.

namespace {

using json = nlohmann::json;

std::optional<json> parse_json(std::string_view _text) {
    try {
        return json::parse(_text);
    } catch (const json::exception &) {
        return std::nullopt;
    }
}

/// Recursively checks every object schema in `_doc`: nodes with
/// "type":"object" must carry "properties" and a "required" array, and every
/// required name must be declared in "properties".
bool check_object_shapes(const json &_doc, const std::string &_path) {
    if (_doc.is_object()) {
        const bool is_object_schema =
            _doc.contains("type") && _doc.at("type").is_string() &&
            _doc.at("type").get<std::string>() == "object";
        if (is_object_schema) {
            if (!_doc.contains("properties") ||
                !_doc.at("properties").is_object()) {
                ADD_FAILURE()
                    << _path << ": \"properties\" is missing or not an object";
                return false;
            }
            if (!_doc.contains("required") || !_doc.at("required").is_array()) {
                ADD_FAILURE()
                    << _path << ": \"required\" is missing or not an array";
                return false;
            }
            for (const auto &name : _doc.at("required")) {
                const std::string key = name.get<std::string>();
                if (!_doc.at("properties").contains(key)) {
                    ADD_FAILURE() << _path << ": \"required\" lists \"" << key
                                  << "\", absent from \"properties\"";
                    return false;
                }
            }
        }
        for (const auto &[key, value] : _doc.items()) {
            if (!check_object_shapes(value, _path + "/" + key)) {
                return false;
            }
        }
    } else if (_doc.is_array()) {
        for (std::size_t i = 0; i < _doc.size(); ++i) {
            if (!check_object_shapes(_doc.at(i),
                                     _path + "/" + std::to_string(i))) {
                return false;
            }
        }
    }
    return true;
}

bool check_schema(const char *_label, std::string_view _text) {
    const auto doc = parse_json(_text);
    if (!doc) {
        ADD_FAILURE() << _label << ": not well-formed JSON";
        return false;
    }
    return check_object_shapes(*doc, _label);
}

/// The OpenAI tool wrapper:
/// {"type":"function","function":{name,description,parameters}}.
bool check_tool_schema(const char *_label, std::string_view _text) {
    const auto doc = parse_json(_text);
    if (!doc) {
        ADD_FAILURE() << _label << ": not well-formed JSON";
        return false;
    }
    if (!doc->contains("type") || doc->at("type") != "function" ||
        !doc->contains("function") || !doc->at("function").is_object()) {
        ADD_FAILURE() << _label << ": missing the OpenAI tool wrapper";
        return false;
    }
    const json &fn = doc->at("function");
    for (const char *key : {"name", "description", "parameters"}) {
        if (!fn.contains(key)) {
            ADD_FAILURE() << _label << ": \"function." << key
                          << "\" is missing";
            return false;
        }
    }
    if (!fn.at("name").is_string() || !fn.at("description").is_string()) {
        ADD_FAILURE() << _label
                      << ": \"function.name\"/\"description\" are not strings";
        return false;
    }
    return check_object_shapes(fn.at("parameters"),
                               std::string(_label) + "/parameters");
}

std::size_t count_descriptions(const json &_doc) {
    std::size_t n = 0;
    if (_doc.is_object()) {
        if (_doc.contains("description")) {
            ++n;
        }
        for (const auto &[key, value] : _doc.items()) {
            n += count_descriptions(value);
        }
    } else if (_doc.is_array()) {
        for (const auto &value : _doc) {
            n += count_descriptions(value);
        }
    }
    return n;
}

void expect_description(const json &_properties, const char *_name,
                        const char *_expected) {
    ASSERT_TRUE(_properties.contains(_name))
        << "property \"" << _name << "\" is missing";
    ASSERT_TRUE(_properties.at(_name).contains("description"))
        << "property \"" << _name << "\" carries no description";
    EXPECT_EQ(_properties.at(_name).at("description").get<std::string>(),
              _expected);
}

/// Runs `_fn` and returns the message of the js::bind_error it throws, or a
/// marker for any other outcome, so a test can assert the error type and its
/// text together: a strict binder must fail loudly, not coerce.
template <typename Fn> std::string bind_error_message(Fn &&_fn) {
    try {
        _fn();
    } catch (const js::bind_error &e) {
        return e.what();
    } catch (const std::exception &e) {
        return std::string("(not a bind_error) ") + e.what();
    }
    return "(no error)";
}

/// Decodes a SearchRequest from `_text`; a malformed fixture surfaces through
/// bind_error_message as a "(not a bind_error)" message rather than crashing.
app::SearchRequest decode_request(std::string_view _text) {
    const auto doc = parse_json(_text);
    if (!doc) {
        throw std::runtime_error("test fixture is not valid JSON");
    }
    return js::from_json<app::SearchRequest>(*doc);
}

constexpr std::string_view fn_schema =
    js::function_schema<^^app::CppReflectServer::search_documents>();
constexpr std::string_view tool_schema =
    js::function_schema<^^app::CppReflectServer::search_documents,
                        js::Style::OpenAiTool>();
constexpr std::string_view query_schema = js::tool_schema<app::Query>();
constexpr std::string_view request_schema =
    js::tool_schema<app::SearchRequest>();

// A shape union. No member carries a default initializer: a defaulted member is
// absent from "required", and an absent field matches every alternative, which
// would make the union undecidable.
struct Rectangle {
    double width;
    double height;
};
struct Circle {
    double radius;
};
using Shape = std::variant<Rectangle, Circle>;
constexpr std::string_view shape_schema =
    std::define_static_string(js::param_schema<Shape>());

} // namespace

TEST(JsonSchema, generated_shapes_are_well_formed) {
    EXPECT_TRUE(check_schema("function schema", fn_schema));
    EXPECT_TRUE(check_tool_schema("openai tool", tool_schema));
    EXPECT_TRUE(check_schema("struct Query", query_schema));
    EXPECT_TRUE(check_schema("struct SearchRequest", request_schema));
}

TEST(JsonSchema, descriptions_come_from_the_annotations) {
    const auto tool = parse_json(tool_schema);
    ASSERT_TRUE(tool.has_value());
    const auto &props = tool->at("function").at("parameters").at("properties");
    expect_description(props, "query", "The raw query text.");
    expect_description(props, "limit", "Maximum number of hits to return.");
    expect_description(props, "exact_match",
                       "Require an exact (case-sensitive) match.");

    const auto query = parse_json(query_schema);
    ASSERT_TRUE(query.has_value());
    expect_description(query->at("properties"), "text", "The raw query text.");
    expect_description(
        query->at("properties").at("ranges").at("items").at("properties"),
        "max", "Optional inclusive upper bound.");

    const auto request = parse_json(request_schema);
    ASSERT_TRUE(request.has_value());
    expect_description(request->at("properties").at("query").at("properties"),
                       "unit", "Temperature unit for numeric filters.");
}

TEST(JsonSchema, schema_annotations_survive_serialization) {
    const auto count_of = [](std::string_view _text) {
        const auto doc = parse_json(_text);
        return doc ? count_descriptions(*doc) : std::size_t{0};
    };
    EXPECT_GT(count_of(fn_schema), 0u);
    EXPECT_GT(count_of(request_schema), 0u);
}

TEST(JsonSchema, from_json_decodes_into_the_struct) {
    const char *request_json = R"({
        "query": {
            "text": "static reflection",
            "unit": "Fahrenheit",
            "ranges": [{"min": 1, "max": 10}, {"min": 20}],
            "fuzzy": true,
            "weight": 0.75
        },
        "top_k": [3, 5, 8]
    })";
    const auto doc = parse_json(request_json);
    ASSERT_TRUE(doc.has_value());

    const app::SearchRequest req = js::from_json<app::SearchRequest>(*doc);
    EXPECT_EQ(req.query.text, "static reflection");
    EXPECT_EQ(req.query.unit, app::Unit::Fahrenheit);
    ASSERT_EQ(req.query.ranges.size(), 2u);
    EXPECT_EQ(req.query.ranges[0].min, 1);
    EXPECT_EQ(req.query.ranges[0].max, std::optional<int>{10});
    EXPECT_EQ(req.query.ranges[1].max, std::nullopt)
        << "an absent optional member stays empty";
    EXPECT_EQ(req.query.weight, std::optional<double>{0.75});
    EXPECT_EQ(req.top_k, (std::array<int, 3>{3, 5, 8}));
    EXPECT_FALSE(req.debug) << "an absent member keeps its default";
}

TEST(JsonSchema, json_arguments_drive_a_real_member_call) {
    app::CppReflectServer server;

    const auto doc =
        parse_json(R"({"query":"reflection","limit":2,"exact_match":true})");
    ASSERT_TRUE(doc.has_value());
    const auto text =
        js::invoke_member_with_json<^^app::CppReflectServer::search_documents>(
            server, *doc);

    // The call reached the real tool, so the payload echoes the arguments and
    // the hits name files the corpus actually has.
    const auto payload = parse_json(text);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->at("query"), "reflection");
    EXPECT_EQ(payload->at("limit"), 2);
    EXPECT_TRUE(payload->at("exact_match").get<bool>());
    ASSERT_FALSE(payload->at("hits").empty())
        << "the corpus yields hits for 'reflection'";
    for (const auto &hit : payload->at("hits")) {
        EXPECT_TRUE(hit.contains("file") && hit.contains("line") &&
                    hit.contains("text"));
    }
}

// A document arrives from a model, so every value must match the reflected type
// exactly: a mistyped field is an error, never a coercion, and the message
// names the field it failed on so the model can correct itself.

TEST(JsonSchema, a_mistyped_scalar_is_rejected) {
    // An int is not a string, and a float is not an int.
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(R"({"query":{"text":42,"ranges":[]}})");
              }),
              "query: text: expected string");
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[{"min":"1"}]}})");
              }),
              "query: ranges: min: expected integer");
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[{"min":1.5}]}})");
              }),
              "query: ranges: min: expected integer");
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[]},"debug":"true"})");
              }),
              "debug: expected boolean");
}

TEST(JsonSchema, a_mistyped_container_is_rejected) {
    // A sequence must be an array and a class must be an object; neither is
    // coerced.
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":"nope"}})");
              }),
              "query: ranges: expected array");
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(R"({"query":{"text":"x","ranges":{}}})");
              }),
              "query: ranges: expected array");
    EXPECT_EQ(
        bind_error_message([] { (void)decode_request(R"({"query":[]})"); }),
        "query: expected object");
}

TEST(JsonSchema, an_array_of_the_wrong_length_is_rejected) {
    // std::array's length is part of its type, so a short document is an error
    // rather than a silently padded result.
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[]},"top_k":[1,2]})");
              }),
              "top_k: array length mismatch: expected 3, got 2");
}

TEST(JsonSchema, a_mistyped_enum_is_rejected) {
    // An enum is a JSON string: a number is not one, and an unknown name is not
    // an enumerator. Both carry the field's path.
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[],"unit":1}})");
              }),
              "query: unit: expected string");
    EXPECT_EQ(bind_error_message([] {
                  (void)decode_request(
                      R"({"query":{"text":"x","ranges":[],"unit":"Kelvin"}})");
              }),
              "query: unit: invalid Unit value: Kelvin");
}

TEST(JsonSchema, a_missing_required_member_is_rejected) {
    // `text` and `ranges` carry no initializer, so they must be present; `unit`
    // and `debug` do, so they may be omitted.
    EXPECT_EQ(bind_error_message(
                  [] { (void)decode_request(R"({"query":{"ranges":[]}})"); }),
              "query: missing required member: text");
    EXPECT_EQ(bind_error_message(
                  [] { (void)decode_request(R"({"query":{"text":"x"}})"); }),
              "query: missing required member: ranges");
}

TEST(JsonSchema, the_tool_call_path_binds_the_same_way) {
    // The function path shares from_json, and an argument with no default and
    // no std::optional must be supplied; a mistyped one names the argument.
    app::CppReflectServer server;

    const auto missing = parse_json(R"({"limit":5,"exact_match":true})");
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(bind_error_message([&] {
                  (void)js::invoke_member_with_json<
                      ^^app::CppReflectServer::search_documents>(server,
                                                                 *missing);
              }),
              "missing required argument: query");

    const auto mistyped =
        parse_json(R"({"query":5,"limit":5,"exact_match":true})");
    ASSERT_TRUE(mistyped.has_value());
    EXPECT_EQ(bind_error_message([&] {
                  (void)js::invoke_member_with_json<
                      ^^app::CppReflectServer::search_documents>(server,
                                                                 *mistyped);
              }),
              "query: expected string");
}

// A std::variant is an anyOf over its alternatives, and decoding one means
// deciding which alternative the document is. Exactly one alternative must fit:
// zero is a document no alternative describes, and two is a document that does
// not say which alternative it is. Neither is guessed at, because a guess lands
// a plausible-looking wrong value -- the failure mode from_json exists to
// prevent.

TEST(JsonSchema, a_variant_is_an_any_of_over_its_alternatives) {
    const auto shape = parse_json(shape_schema);
    ASSERT_TRUE(shape.has_value());
    const json &any_of = shape->at("anyOf");
    ASSERT_TRUE(any_of.is_array());
    ASSERT_EQ(any_of.size(), 2u) << "one branch per alternative";
    // Order follows the declaration, and each branch is the alternative's own
    // object schema.
    EXPECT_EQ(any_of.at(0).at("required"), json::array({"width", "height"}));
    EXPECT_EQ(any_of.at(1).at("required"), json::array({"radius"}));
    EXPECT_FALSE(shape->contains("oneOf"))
        << "oneOf is rejected by OpenAI and Anthropic tool schemas";
}

TEST(JsonSchema, a_variant_decodes_the_alternative_the_document_names) {
    const auto rect = parse_json(R"({"width":3,"height":4})");
    ASSERT_TRUE(rect.has_value());
    const Shape as_rect = js::from_json<Shape>(*rect);
    ASSERT_TRUE(std::holds_alternative<Rectangle>(as_rect));
    EXPECT_DOUBLE_EQ(std::get<Rectangle>(as_rect).width, 3.0);
    EXPECT_DOUBLE_EQ(std::get<Rectangle>(as_rect).height, 4.0);

    const auto circle = parse_json(R"({"radius":2.5})");
    ASSERT_TRUE(circle.has_value());
    const Shape as_circle = js::from_json<Shape>(*circle);
    ASSERT_TRUE(std::holds_alternative<Circle>(as_circle));
    EXPECT_DOUBLE_EQ(std::get<Circle>(as_circle).radius, 2.5);
}

TEST(JsonSchema, a_variant_round_trips_through_to_json) {
    // The active alternative is preserved, and no tag field is invented.
    const json rect{{"width", 1.0}, {"height", 2.0}};
    const Shape shape = js::from_json<Shape>(rect);
    EXPECT_EQ(json::parse(js::to_json(shape)), rect);
}

TEST(JsonSchema, an_ambiguous_variant_document_is_rejected) {
    // Both alternatives' members are present, so both fit and the document does
    // not say which it is.
    const auto both = parse_json(R"({"width":1,"height":2,"radius":3})");
    ASSERT_TRUE(both.has_value());
    EXPECT_EQ(bind_error_message([&] { (void)js::from_json<Shape>(*both); }),
              "variant is ambiguous: 2 alternatives matched (Rectangle, "
              "Circle)");
}

TEST(JsonSchema, a_variant_document_matching_no_alternative_is_rejected) {
    // Every alternative's failure is reported, so the model can correct itself
    // rather than guess at which one was wrong.
    const auto none = parse_json(R"({"radius":"thick"})");
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(
        bind_error_message([&] { (void)js::from_json<Shape>(*none); }),
        "no variant alternative matched: Rectangle: missing required member: "
        "width, Circle: radius: expected number");
}

TEST(JsonSchema, the_consumer_learns_which_alternative_is_active) {
    // The decoded variant tells its consumer exactly what it holds, using only
    // std::variant's own facilities. This is sound because of the
    // exactly-one-match rule: a document fitting zero or several alternatives
    // throws before any value is handed over, so there is never a variant whose
    // element type is unclear.
    const auto rect = parse_json(R"({"width":3,"height":4})");
    const auto circle = parse_json(R"({"radius":2.5})");
    ASSERT_TRUE(rect.has_value());
    ASSERT_TRUE(circle.has_value());

    const Shape as_rect = js::from_json<Shape>(*rect);
    const Shape as_circle = js::from_json<Shape>(*circle);

    // std::holds_alternative answers "which alternative is in there".
    EXPECT_TRUE(std::holds_alternative<Rectangle>(as_rect));
    EXPECT_FALSE(std::holds_alternative<Circle>(as_rect));
    EXPECT_TRUE(std::holds_alternative<Circle>(as_circle));

    // std::visit dispatches to that alternative's handler, so the consumer
    // reads the members its actual type has and no others.
    const auto area = [](const Shape &_shape) {
        return std::visit(
            []<typename T>(const T &_s) -> double {
                if constexpr (std::is_same_v<T, Rectangle>) {
                    return _s.width * _s.height;
                } else {
                    return 3.141592653589793 * _s.radius * _s.radius;
                }
            },
            _shape);
    };
    EXPECT_DOUBLE_EQ(area(as_rect), 12.0);
    EXPECT_DOUBLE_EQ(area(as_circle), 3.141592653589793 * 6.25);
}

TEST(JsonSchema, a_scalar_union_stays_unambiguous) {
    // Alternatives of different JSON types never overlap, so this is the case
    // that needs no tag and can never be ambiguous.
    using Scalar = std::variant<int, std::string>;
    EXPECT_EQ(js::from_json<Scalar>(json(7)), Scalar{7});
    EXPECT_EQ(js::from_json<Scalar>(json("seven")), Scalar{"seven"});
}
