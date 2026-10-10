#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "scaffold/app_server.h"
#include "llm/reflection_json.h"

// JSON Schema generated from C++26 static reflection, validated with a real JSON
// parser (nlohmann/json). The schemas are consteval strings, but their content is
// only meaningful once parsed, so these are runtime EXPECT checks.

namespace {

using json = nlohmann::json;

std::optional<json> parse_json(std::string_view _text)
{
    try {
        return json::parse(_text);
    } catch (const json::exception&) {
        return std::nullopt;
    }
}

/// Recursively checks every object schema in `_doc`: nodes with
/// "type":"object" must carry "properties" and a "required" array, and every
/// required name must be declared in "properties".
bool check_object_shapes(const json& _doc, const std::string& _path)
{
    if (_doc.is_object()) {
        const bool is_object_schema = _doc.contains("type") && _doc.at("type").is_string() &&
                                      _doc.at("type").get<std::string>() == "object";
        if (is_object_schema) {
            if (!_doc.contains("properties") || !_doc.at("properties").is_object()) {
                ADD_FAILURE() << _path << ": \"properties\" is missing or not an object";
                return false;
            }
            if (!_doc.contains("required") || !_doc.at("required").is_array()) {
                ADD_FAILURE() << _path << ": \"required\" is missing or not an array";
                return false;
            }
            for (const auto& name : _doc.at("required")) {
                const std::string key = name.get<std::string>();
                if (!_doc.at("properties").contains(key)) {
                    ADD_FAILURE() << _path << ": \"required\" lists \"" << key
                                  << "\", absent from \"properties\"";
                    return false;
                }
            }
        }
        for (const auto& [key, value] : _doc.items()) {
            if (!check_object_shapes(value, _path + "/" + key)) {
                return false;
            }
        }
    } else if (_doc.is_array()) {
        for (std::size_t i = 0; i < _doc.size(); ++i) {
            if (!check_object_shapes(_doc.at(i), _path + "/" + std::to_string(i))) {
                return false;
            }
        }
    }
    return true;
}

bool check_schema(const char* _label, std::string_view _text)
{
    const auto doc = parse_json(_text);
    if (!doc) {
        ADD_FAILURE() << _label << ": not well-formed JSON";
        return false;
    }
    return check_object_shapes(*doc, _label);
}

/// The OpenAI tool wrapper: {"type":"function","function":{name,description,parameters}}.
bool check_tool_schema(const char* _label, std::string_view _text)
{
    const auto doc = parse_json(_text);
    if (!doc) {
        ADD_FAILURE() << _label << ": not well-formed JSON";
        return false;
    }
    if (!doc->contains("type") || doc->at("type") != "function" || !doc->contains("function") ||
        !doc->at("function").is_object()) {
        ADD_FAILURE() << _label << ": missing the OpenAI tool wrapper";
        return false;
    }
    const json& fn = doc->at("function");
    for (const char* key : {"name", "description", "parameters"}) {
        if (!fn.contains(key)) {
            ADD_FAILURE() << _label << ": \"function." << key << "\" is missing";
            return false;
        }
    }
    if (!fn.at("name").is_string() || !fn.at("description").is_string()) {
        ADD_FAILURE() << _label << ": \"function.name\"/\"description\" are not strings";
        return false;
    }
    return check_object_shapes(fn.at("parameters"), std::string(_label) + "/parameters");
}

std::size_t count_descriptions(const json& _doc)
{
    std::size_t n = 0;
    if (_doc.is_object()) {
        if (_doc.contains("description")) {
            ++n;
        }
        for (const auto& [key, value] : _doc.items()) {
            n += count_descriptions(value);
        }
    } else if (_doc.is_array()) {
        for (const auto& value : _doc) {
            n += count_descriptions(value);
        }
    }
    return n;
}

void expect_description(const json& _properties, const char* _name, const char* _expected)
{
    ASSERT_TRUE(_properties.contains(_name)) << "property \"" << _name << "\" is missing";
    ASSERT_TRUE(_properties.at(_name).contains("description"))
        << "property \"" << _name << "\" carries no description";
    EXPECT_EQ(_properties.at(_name).at("description").get<std::string>(), _expected);
}

constexpr std::string_view fn_schema = js::function_schema<^^app::CppReflectServer::search_documents>();
constexpr std::string_view tool_schema =
    js::function_schema<^^app::CppReflectServer::search_documents, js::Style::OpenAiTool>();
constexpr std::string_view query_schema = js::tool_schema<app::Query>();
constexpr std::string_view request_schema = js::tool_schema<app::SearchRequest>();

} // namespace

TEST(JsonSchema, generated_shapes_are_well_formed)
{
    EXPECT_TRUE(check_schema("function schema", fn_schema));
    EXPECT_TRUE(check_tool_schema("openai tool", tool_schema));
    EXPECT_TRUE(check_schema("struct Query", query_schema));
    EXPECT_TRUE(check_schema("struct SearchRequest", request_schema));
}

TEST(JsonSchema, descriptions_come_from_the_annotations)
{
    const auto tool = parse_json(tool_schema);
    ASSERT_TRUE(tool.has_value());
    const auto& props = tool->at("function").at("parameters").at("properties");
    expect_description(props, "query", "The raw query text.");
    expect_description(props, "limit", "Maximum number of hits to return.");
    expect_description(props, "exact_match", "Require an exact match.");
    expect_description(props, "unit", "Optional unit filter.");

    const auto query = parse_json(query_schema);
    ASSERT_TRUE(query.has_value());
    expect_description(query->at("properties"), "text", "The raw query text.");
    expect_description(query->at("properties").at("ranges").at("items").at("properties"), "max",
                       "Optional inclusive upper bound.");

    const auto request = parse_json(request_schema);
    ASSERT_TRUE(request.has_value());
    expect_description(request->at("properties").at("query").at("properties"), "unit",
                       "Temperature unit for numeric filters.");
}

TEST(JsonSchema, schema_annotations_survive_serialization)
{
    const auto count_of = [](std::string_view _text) {
        const auto doc = parse_json(_text);
        return doc ? count_descriptions(*doc) : std::size_t{0};
    };
    EXPECT_GT(count_of(fn_schema), 0u);
    EXPECT_GT(count_of(request_schema), 0u);
}

TEST(JsonSchema, from_json_decodes_into_the_struct)
{
    const char* request_json = R"({
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
    EXPECT_EQ(req.query.ranges[1].max, std::nullopt) << "an absent optional member stays empty";
    EXPECT_EQ(req.query.weight, std::optional<double>{0.75});
    EXPECT_EQ(req.top_k, (std::array<int, 3>{3, 5, 8}));
    EXPECT_FALSE(req.debug) << "an absent member keeps its default";
}

TEST(JsonSchema, json_arguments_drive_a_real_member_call)
{
    app::CppReflectServer server;

    const auto full_doc = parse_json(R"({"query":"reflection in C++26","limit":5,
                                         "exact_match":true,"unit":"Fahrenheit"})");
    ASSERT_TRUE(full_doc.has_value());
    const std::string full =
        js::invoke_member_with_json<^^app::CppReflectServer::search_documents>(server, *full_doc);
    EXPECT_NE(full.find("\"unit\":\"Fahrenheit\""), std::string::npos)
        << "an explicit optional argument reaches the C++ call";

    const auto omitted_doc = parse_json(R"({"query":"hello","limit":1,"exact_match":false})");
    ASSERT_TRUE(omitted_doc.has_value());
    const std::string omitted =
        js::invoke_member_with_json<^^app::CppReflectServer::search_documents>(server, *omitted_doc);
    EXPECT_EQ(omitted.find("\"unit\""), std::string::npos)
        << "an omitted optional argument stays unset";
}

TEST(JsonSchema, an_out_of_range_enum_is_rejected)
{
    app::CppReflectServer server;
    const auto doc = parse_json(R"({"query":"x","limit":1,"exact_match":true,"unit":"Kelvin"})");
    ASSERT_TRUE(doc.has_value());

    EXPECT_THROW(
        (void)js::invoke_member_with_json<^^app::CppReflectServer::search_documents>(server, *doc),
        std::exception);
}
