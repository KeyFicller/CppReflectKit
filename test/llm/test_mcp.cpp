#include <cstddef>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "scaffold/app_server.h"

// Drives the real serve() loop through string streams, so what is asserted is
// exactly what an MCP client receives: one reply line per request. The server
// itself is mcp.h + app_server.h; the stdio entry is mcp_server.cpp and the
// CLI/socket bridge is bridge.cpp.

namespace {

using json = nlohmann::json;

/// Runs newline-delimited requests through a fresh server and returns the reply
/// lines. A request without an id produces no line, which is how the notification
/// case is observed.
std::vector<json> drive(const std::vector<std::string>& _requests)
{
    std::string input;
    for (const std::string& request : _requests) {
        input += request;
        input += '\n';
    }
    std::istringstream in{input};
    std::ostringstream out;
    app::CppReflectServer server;
    server.serve(in, out);

    std::vector<json> replies;
    std::istringstream lines{out.str()};
    std::string line;
    while (std::getline(lines, line)) {
        replies.push_back(json::parse(line));
    }
    return replies;
}

json request(std::string_view _method, json _params = json::object())
{
    json r{{"jsonrpc", "2.0"}, {"id", 1}, {"method", _method}};
    if (!_params.empty()) {
        r["params"] = std::move(_params);
    }
    return r;
}

void check_eq(const json& _got, const json& _want, std::string_view _what)
{
    EXPECT_EQ(_got, _want) << _what;
}

/// The `result` object of the single reply to `_request`.
json result_of(std::string_view _request)
{
    return drive({std::string(_request)}).at(0).at("result");
}

} // namespace

TEST(Mcp, server_discover)
{
    const json result = result_of(request("server/discover").dump());
    check_eq(result.at("supportedVersions"), json::array({mcp::kProtocolVersion}),
             "advertises the pinned revision");
    const json& caps = result.at("capabilities");
    EXPECT_TRUE(caps.contains("tools") && caps.contains("prompts") && caps.contains("resources"))
        << "capabilities cover tools, prompts and resources";
    check_eq(result.at("_meta").at("io.modelcontextprotocol/serverInfo").at("name"), "cppreflect",
             "serverInfo.name");
}

TEST(Mcp, tools_list)
{
    const json tools = result_of(request("tools/list").dump()).at("tools");
    ASSERT_EQ(tools.size(), 1u) << "exactly the marked function is registered";
    const json& tool = tools.at(0);
    check_eq(tool.at("name"), "search_documents", "name from identifier_of");
    check_eq(tool.at("description"), "Searches the document index.", "description from js::doc");
    const json& schema = tool.at("inputSchema");
    check_eq(schema.at("required"), json::array({"query", "limit", "exact_match"}),
             "required = the non-optional parameters");
    check_eq(schema.at("properties").at("query").at("description"), "The raw query text.",
             "parameter description from js::param_docs");
    EXPECT_EQ(schema.at("properties").at("unit").at("anyOf").size(), 2u)
        << "std::optional parameter -> anyOf[enum, null]";
}

TEST(Mcp, tools_call)
{
    const json result = result_of(request("tools/call",
                                          {{"name", "search_documents"},
                                           {"arguments", {{"query", "mcp"},
                                                          {"limit", 2},
                                                          {"exact_match", true},
                                                          {"unit", "Fahrenheit"}}}})
                                     .dump());
    check_eq(result.at("isError"), false, "valid call is not an error");
    check_eq(result.at("content").at(0).at("type"), "text", "content is a text block");
    EXPECT_NE(result.at("content").at(0).at("text").get<std::string>().find("mcp #1"),
              std::string::npos)
        << "the reflected call produced its payload";
}

TEST(Mcp, a_failing_tool_is_a_result_not_a_transport_error)
{
    // A tool that throws is a *result* with isError, not a JSON-RPC error, so the
    // model can self-correct instead of the client seeing a transport fault.
    const json reply =
        drive({request("tools/call", {{"name", "search_documents"}, {"arguments", {{"query", "x"}}}})
                   .dump()})
            .at(0);
    EXPECT_FALSE(reply.contains("error")) << "a failing tool is not a JSON-RPC error";
    check_eq(reply.at("result").at("isError"), true, "missing argument -> isError:true");
}

TEST(Mcp, unknown_tool_is_invalid_params)
{
    const json reply =
        drive({request("tools/call", {{"name", "nosuch"}, {"arguments", json::object()}}).dump()})
            .at(0);
    check_eq(reply.at("error").at("code"), -32602, "unknown tool -> -32602");
}

TEST(Mcp, prompts)
{
    const json prompts = result_of(request("prompts/list").dump()).at("prompts");
    ASSERT_EQ(prompts.size(), 1u) << "exactly the marked function is registered";
    const json& prompt = prompts.at(0);
    check_eq(prompt.at("name"), "explain", "name from identifier_of");
    check_eq(prompt.at("description"), "Explain a reflection concept to a newcomer.",
             "description from js::doc");
    const json& argument = prompt.at("arguments").at(0);
    check_eq(argument.at("name"), "topic", "PromptArgument.name is the C++ parameter name");
    check_eq(argument.at("required"), true, "non-optional parameter -> required:true");
    check_eq(argument.at("description"), "The concept to explain, e.g. \"template for\".",
             "PromptArgument.description from js::param_docs");

    const json result = result_of(
        request("prompts/get", {{"name", "explain"}, {"arguments", {{"topic", "P2996"}}}}).dump());
    check_eq(result.at("messages").at(0).at("role"), "user", "message role");
    EXPECT_NE(result.at("messages")
                  .at(0)
                  .at("content")
                  .at("text")
                  .get<std::string>()
                  .find("P2996"),
              std::string::npos)
        << "the prompt body received its argument";
}

TEST(Mcp, prompt_errors)
{
    const json missing_arg =
        drive({request("prompts/get", {{"name", "explain"}, {"arguments", json::object()}}).dump()})
            .at(0);
    check_eq(missing_arg.at("error").at("code"), -32602,
             "missing required prompt argument -> -32602");

    const json unknown = drive({request("prompts/get", {{"name", "nosuch"}}).dump()}).at(0);
    check_eq(unknown.at("error").at("code"), -32602, "unknown prompt -> -32602");
}

TEST(Mcp, resources)
{
    const json resources = result_of(request("resources/list").dump()).at("resources");
    ASSERT_EQ(resources.size(), 1u) << "exactly the marked function is registered";
    const json& resource = resources.at(0);
    check_eq(resource.at("uri"), "file:///docs/mcp_automation.md", "uri read from the marker");
    check_eq(resource.at("mimeType"), "text/markdown", "mimeType read from the marker");
    check_eq(resource.at("name"), "design_notes", "name from identifier_of");
    check_eq(resource.at("description"), "Design notes for the reflection-driven MCP server.",
             "description from js::doc");

    const json result =
        result_of(request("resources/read", {{"uri", "file:///docs/mcp_automation.md"}}).dump());
    const json& contents = result.at("contents");
    ASSERT_EQ(contents.size(), 1u) << "one content entry";
    check_eq(contents.at(0).at("mimeType"), "text/markdown", "content mimeType");
    EXPECT_NE(contents.at(0).at("text").get<std::string>().find("Reflection-driven MCP"),
              std::string::npos)
        << "the resource body was invoked";

    const json missing =
        drive({request("resources/read", {{"uri", "file:///nope"}}).dump()}).at(0);
    check_eq(missing.at("error").at("code"), -32602,
             "missing resource -> -32602 (never empty contents)");
}

TEST(Mcp, protocol_edges)
{
    const std::vector<json> replies =
        drive({R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{}})"});
    EXPECT_TRUE(replies.empty()) << "a notification (no id) draws no reply";

    const json malformed = drive({"not json"}).at(0);
    check_eq(malformed.at("error").at("code"), -32700, "malformed JSON -> -32700");

    const json unknown_method = drive({request("tools/unknown").dump()}).at(0);
    check_eq(unknown_method.at("error").at("code"), -32601, "unknown method -> -32601");

    json mismatched = request("tools/list");
    mismatched["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "2025-06-18";
    const json version_reply = drive({mismatched.dump()}).at(0);
    check_eq(version_reply.at("error").at("code"), -32022,
             "a mismatched protocol version -> -32022");
}

TEST(Mcp, one_table_two_emitters)
{
    app::CppReflectServer server;
    const json openai = json::parse(server.tools_as_openai_json());
    const json listed = result_of(request("tools/list").dump()).at("tools");
    check_eq(openai.size(), listed.size(), "--emit-tools and MCP tools/list read one table");
    check_eq(openai.at(0).at("function").at("name"), listed.at(0).at("name"),
             "same name from both emitters");
    check_eq(openai.at(0).at("function").at("parameters"), listed.at(0).at("inputSchema"),
             "same inputSchema behind the OpenAI wrapper");
    EXPECT_NE(server.call_tool("search_documents",
                               {{"query", "q"}, {"limit", 1}, {"exact_match", false}})
                  .find("q #1"),
              std::string::npos)
        << "call_tool mirrors the MCP call path";

    try {
        (void)server.call_tool("nosuch", json::object());
        ADD_FAILURE() << "call_tool should reject an unknown tool";
    } catch (const std::exception& e) {
        EXPECT_EQ(std::string(e.what()), "unknown tool: nosuch")
            << "call_tool keeps the CLI error wording";
    }
}
