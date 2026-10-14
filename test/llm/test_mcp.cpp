#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
/// lines. A request without an id produces no line, which is how the
/// notification case is observed.
std::vector<json> drive(const std::vector<std::string> &_requests) {
    std::string input;
    for (const std::string &request : _requests) {
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

json request(std::string_view _method, json _params = json::object()) {
    json r{{"jsonrpc", "2.0"}, {"id", 1}, {"method", _method}};
    if (!_params.empty()) {
        r["params"] = std::move(_params);
    }
    return r;
}

void check_eq(const json &_got, const json &_want, std::string_view _what) {
    EXPECT_EQ(_got, _want) << _what;
}

/// The `result` object of the single reply to `_request`.
json result_of(std::string_view _request) {
    return drive({std::string(_request)}).at(0).at("result");
}

} // namespace

TEST(Mcp, server_discover) {
    const json result = result_of(request("server/discover").dump());
    check_eq(result.at("supportedVersions"),
             json::array({mcp::kProtocolVersion}),
             "advertises the pinned revision");
    const json &caps = result.at("capabilities");
    EXPECT_TRUE(caps.contains("tools") && caps.contains("prompts") &&
                caps.contains("resources"))
        << "capabilities cover tools, prompts and resources";
    check_eq(
        result.at("_meta").at("io.modelcontextprotocol/serverInfo").at("name"),
        "cppreflect", "serverInfo.name");
}

TEST(Mcp, ping_is_answered) {
    const json reply = drive({request("ping").dump()}).at(0);
    EXPECT_FALSE(reply.contains("error"))
        << "ping is a liveness probe, not an unknown method";
    check_eq(reply.at("result")
                 .at("_meta")
                 .at("io.modelcontextprotocol/serverInfo")
                 .at("name"),
             "cppreflect", "ping results carry serverInfo like the others");
}

TEST(Mcp, initialize_pins_a_stdio_connection_to_the_handshake_era) {
    // One connection, so the handshake the first line opens applies to the
    // lines after it: none of them carries the modern result metadata.
    const std::vector<json> replies = drive(
        {request("initialize",
                 {{"protocolVersion", std::string(mcp::kHandshakeVersion)}})
             .dump(),
         request("tools/list").dump(), request("ping").dump()});
    ASSERT_EQ(replies.size(), 3u);
    const json &listed = replies.at(1).at("result");
    EXPECT_FALSE(listed.contains("_meta"))
        << "a pinned connection gets no per-result serverInfo";
    EXPECT_FALSE(listed.contains("resultType"));
    EXPECT_FALSE(listed.contains("ttlMs"));
    EXPECT_FALSE(listed.contains("cacheScope"));
    // The result itself is untouched: only the modern metadata is gone.
    EXPECT_EQ(listed.at("tools").at(0).at("name"), "search_documents");
    EXPECT_FALSE(replies.at(2).at("result").contains("_meta"))
        << "every later line is answered in the same dialect";
}

TEST(Mcp, legacy_initialize_handshake) {
    const std::string handshake{mcp::kHandshakeVersion};
    const json agreed = result_of(
        request("initialize", {{"protocolVersion", handshake},
                               {"capabilities", json::object()},
                               {"clientInfo", json{{"name", "cursor"},
                                                   {"version", "1.25.1"}}}})
            .dump());
    check_eq(agreed.at("protocolVersion"), handshake,
             "echoes the revision we answer");
    check_eq(agreed.at("serverInfo").at("name"), "cppreflect",
             "identifies itself");
    EXPECT_TRUE(agreed.at("capabilities").contains("tools"))
        << "capabilities still travel";
    EXPECT_FALSE(agreed.contains("_meta"))
        << "the handshake predates result metadata";
    EXPECT_FALSE(agreed.contains("resultType"));

    // Anything older is answered at the newest handshake revision, never served
    // in a dialect this server does not implement.
    const json corrected = result_of(
        request("initialize", {{"protocolVersion", "2024-11-05"}}).dump());
    check_eq(corrected.at("protocolVersion"), handshake,
             "an older revision is corrected");

    // notifications/initialized is a notification: no reply line.
    EXPECT_TRUE(
        drive(
            {R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})"})
            .empty())
        << "the initialized notification draws no reply";
}

TEST(Mcp, tools_list) {
    const json listed = result_of(request("tools/list").dump());
    check_eq(listed.at("_meta").at("io.modelcontextprotocol/serverInfo"),
             json{{"name", "cppreflect"}, {"version", "0.1.0"}},
             "every result carries serverInfo");
    const json tools = listed.at("tools");
    ASSERT_EQ(tools.size(), 1u) << "exactly the marked function is registered";
    const json &tool = tools.at(0);
    check_eq(tool.at("name"), "search_documents", "name from identifier_of");
    check_eq(tool.at("description"),
             "Searches the repository's own documentation.",
             "description from js::doc");
    const json &schema = tool.at("inputSchema");
    check_eq(schema.at("required"),
             json::array({"query", "limit", "exact_match"}),
             "required = the non-optional parameters");
    check_eq(schema.at("properties").at("query").at("description"),
             "The raw query text.",
             "parameter description from js::param_docs");
    EXPECT_EQ(schema.at("properties").size(), 3u)
        << "the signature's three parameters";
    check_eq(tool.at("annotations"),
             json{{"readOnlyHint", true},
                  {"destructiveHint", false},
                  {"idempotentHint", false},
                  {"openWorldHint", false}},
             "hints from the marker; a read-only tool is never destructive");
}

TEST(Mcp, tools_call) {
    const json result = result_of(
        request(
            "tools/call",
            {{"name", "search_documents"},
             {"arguments",
              {{"query", "reflection"}, {"limit", 2}, {"exact_match", true}}}})
            .dump());
    check_eq(result.at("isError"), false, "valid call is not an error");
    check_eq(result.at("content").at(0).at("type"), "text",
             "content is a text block");

    // The tool reads the real corpus, so every hit names a file that exists and
    // a line that really contains the query.
    const json payload =
        json::parse(result.at("content").at(0).at("text").get<std::string>());
    const json &hits = payload.at("hits");
    ASSERT_EQ(hits.size(), 2u)
        << "the corpus has more than `limit` matches for 'reflection'";
    for (const json &hit : hits) {
        const std::filesystem::path path =
            std::filesystem::path(CPPREF_CORPUS_ROOT) /
            hit.at("file").get<std::string>();
        EXPECT_TRUE(std::filesystem::exists(path))
            << "hit points at a real file: " << path;
        EXPECT_NE(hit.at("text").get<std::string>().find("reflection"),
                  std::string::npos)
            << "an exact-match hit really contains the query";
    }
}

TEST(Mcp, a_failing_tool_is_a_result_not_a_transport_error) {
    // A tool that throws is a *result* with isError, not a JSON-RPC error, so
    // the model can self-correct instead of the client seeing a transport
    // fault.
    const json reply =
        drive({request("tools/call", {{"name", "search_documents"},
                                      {"arguments", {{"query", "x"}}}})
                   .dump()})
            .at(0);
    EXPECT_FALSE(reply.contains("error"))
        << "a failing tool is not a JSON-RPC error";
    check_eq(reply.at("result").at("isError"), true,
             "missing argument -> isError:true");
    check_eq(reply.at("result")
                 .at("_meta")
                 .at("io.modelcontextprotocol/serverInfo")
                 .at("name"),
             "cppreflect",
             "isError is still a result, so it carries serverInfo");
}

TEST(Mcp, unknown_tool_is_invalid_params) {
    const json reply =
        drive({request("tools/call",
                       {{"name", "nosuch"}, {"arguments", json::object()}})
                   .dump()})
            .at(0);
    check_eq(reply.at("error").at("code"), -32602, "unknown tool -> -32602");
}

TEST(Mcp, prompts) {
    const json prompts =
        result_of(request("prompts/list").dump()).at("prompts");
    ASSERT_EQ(prompts.size(), 1u)
        << "exactly the marked function is registered";
    const json &prompt = prompts.at(0);
    check_eq(prompt.at("name"), "explain", "name from identifier_of");
    check_eq(prompt.at("description"),
             "Explain a reflection concept to a newcomer.",
             "description from js::doc");
    const json &argument = prompt.at("arguments").at(0);
    check_eq(argument.at("name"), "topic",
             "PromptArgument.name is the C++ parameter name");
    check_eq(argument.at("required"), true,
             "non-optional parameter -> required:true");
    check_eq(argument.at("description"),
             "The concept to explain, e.g. \"template for\".",
             "PromptArgument.description from js::param_docs");

    const json result =
        result_of(request("prompts/get", {{"name", "explain"},
                                          {"arguments", {{"topic", "P2996"}}}})
                      .dump());
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

TEST(Mcp, prompt_errors) {
    const json missing_arg =
        drive({request("prompts/get",
                       {{"name", "explain"}, {"arguments", json::object()}})
                   .dump()})
            .at(0);
    check_eq(missing_arg.at("error").at("code"), -32602,
             "missing required prompt argument -> -32602");

    const json unknown =
        drive({request("prompts/get", {{"name", "nosuch"}}).dump()}).at(0);
    check_eq(unknown.at("error").at("code"), -32602,
             "unknown prompt -> -32602");
}

TEST(Mcp, resources) {
    const json resources =
        result_of(request("resources/list").dump()).at("resources");
    ASSERT_EQ(resources.size(), 1u)
        << "exactly the marked function is registered";
    const json &resource = resources.at(0);
    check_eq(resource.at("uri"), "file:///docs/about_llm.md",
             "uri read from the marker");
    check_eq(resource.at("mimeType"), "text/markdown",
             "mimeType read from the marker");
    check_eq(resource.at("name"), "design_notes", "name from identifier_of");
    check_eq(resource.at("description"),
             "Design notes for the reflection-driven MCP server.",
             "description from js::doc");

    const json result = result_of(
        request("resources/read", {{"uri", "file:///docs/about_llm.md"}})
            .dump());
    const json &contents = result.at("contents");
    ASSERT_EQ(contents.size(), 1u) << "one content entry";
    check_eq(contents.at(0).at("mimeType"), "text/markdown",
             "content mimeType");
    // The body serves the real file, so its own heading comes back verbatim.
    const std::string text = contents.at(0).at("text").get<std::string>();
    EXPECT_NE(text.find("Exposing existing C++ to an LLM"), std::string::npos)
        << "the resource serves the file the URI names, not a copy";

    const json missing =
        drive({request("resources/read", {{"uri", "file:///nope"}}).dump()})
            .at(0);
    check_eq(missing.at("error").at("code"), -32602,
             "missing resource -> -32602 (never empty contents)");
}

TEST(Mcp, protocol_edges) {
    const std::vector<json> replies = drive(
        {R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{}})"});
    EXPECT_TRUE(replies.empty()) << "a notification (no id) draws no reply";

    const json malformed = drive({"not json"}).at(0);
    check_eq(malformed.at("error").at("code"), -32700,
             "malformed JSON -> -32700");

    const json unknown_method = drive({request("tools/unknown").dump()}).at(0);
    check_eq(unknown_method.at("error").at("code"), -32601,
             "unknown method -> -32601");
    EXPECT_FALSE(unknown_method.contains("result"))
        << "a JSON-RPC error carries no serverInfo";

    json mismatched = request("tools/list");
    mismatched["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] =
        "2025-06-18";
    const json version_reply = drive({mismatched.dump()}).at(0);
    check_eq(version_reply.at("error").at("code"), -32022,
             "a mismatched protocol version -> -32022");
}

TEST(Mcp, one_table_two_emitters) {
    app::CppReflectServer server;
    const json openai = json::parse(server.tools_as_openai_json());
    const json listed = result_of(request("tools/list").dump()).at("tools");
    check_eq(openai.size(), listed.size(),
             "--emit-tools and MCP tools/list read one table");
    check_eq(openai.at(0).at("function").at("name"), listed.at(0).at("name"),
             "same name from both emitters");
    check_eq(openai.at(0).at("function").at("parameters"),
             listed.at(0).at("inputSchema"),
             "same inputSchema behind the OpenAI wrapper");
    EXPECT_NE(server
                  .call_tool("search_documents", {{"query", "reflection"},
                                                  {"limit", 1},
                                                  {"exact_match", true}})
                  .find("\"query\":\"reflection\""),
              std::string::npos)
        << "call_tool mirrors the MCP call path";

    try {
        (void)server.call_tool("nosuch", json::object());
        ADD_FAILURE() << "call_tool should reject an unknown tool";
    } catch (const std::exception &e) {
        EXPECT_EQ(std::string(e.what()), "unknown tool: nosuch")
            << "call_tool keeps the CLI error wording";
    }
}

namespace {

/// One Streamable HTTP server for the cases below. The destructor stops it,
/// including when an assertion fails.
struct HttpServer {
    app::CppReflectServer server;
    std::thread thread;

    HttpServer() : thread([this] { server.serve_http(0); }) {
        for (int i = 0; i < 100 && server.http_port() < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~HttpServer() {
        server.stop_http();
        if (thread.joinable()) {
            thread.join();
        }
    }

    httplib::Result post(const json &_body, httplib::Headers _headers) {
        _headers.emplace("Accept", "application/json, text/event-stream");
        _headers.emplace("MCP-Protocol-Version",
                         std::string(mcp::kProtocolVersion));
        if (_body.contains("method")) {
            _headers.emplace("Mcp-Method",
                             _body.at("method").get<std::string>());
        }
        httplib::Client client("127.0.0.1", server.http_port());
        return client.Post("/mcp", _headers, _body.dump(), "application/json");
    }
};

json meta_version(std::string_view _version = mcp::kProtocolVersion) {
    return json{
        {"_meta",
         {{"io.modelcontextprotocol/protocolVersion", std::string(_version)}}}};
}

} // namespace

TEST(Mcp, http_serves_tools_list) {
    HttpServer http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";

    json body = request("tools/list");
    body["params"] = meta_version();
    const httplib::Result response = http.post(body, {});
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const json rpc = json::parse(response->body);
    EXPECT_EQ(rpc.at("result")
                  .at("_meta")
                  .at("io.modelcontextprotocol/serverInfo")
                  .at("name"),
              "cppreflect");
    EXPECT_EQ(rpc.at("result").at("tools").at(0).at("name"),
              "search_documents");
}

TEST(Mcp, http_serves_a_handshake_era_request) {
    HttpServer http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";

    // No Mcp-Method and no MCP-Protocol-Version: that absence is what marks the
    // request as the older era, which must be served rather than bounced.
    const json init =
        request("initialize",
                {{"protocolVersion", std::string(mcp::kHandshakeVersion)}});
    httplib::Client client("127.0.0.1", http.server.http_port());
    const httplib::Result response = client.Post(
        "/mcp",
        httplib::Headers{{"Accept", "application/json, text/event-stream"}},
        init.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(json::parse(response->body).at("result").at("protocolVersion"),
              std::string(mcp::kHandshakeVersion));

    // After the handshake that same client sends MCP-Protocol-Version alone --
    // it knows nothing of Mcp-Method, so the request still is not strict.
    const json listed = request("tools/list");
    const httplib::Result after = client.Post(
        "/mcp",
        httplib::Headers{
            {"Accept", "application/json, text/event-stream"},
            {"MCP-Protocol-Version", std::string(mcp::kHandshakeVersion)}},
        listed.dump(), "application/json");
    ASSERT_TRUE(after);
    EXPECT_EQ(after->status, 200);
    EXPECT_EQ(
        json::parse(after->body).at("result").at("tools").at(0).at("name"),
        "search_documents");
}

TEST(Mcp, http_answers_the_handshake_era_without_result_metadata) {
    HttpServer http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";

    // No Mcp-Method: the request is from the handshake era, which predates
    // _meta.serverInfo, the resultType tag and the cache fields. Every POST is
    // its own connection, so the header is the only thing that declares this.
    const json listed = request("tools/list");
    httplib::Client client("127.0.0.1", http.server.http_port());
    const httplib::Result response = client.Post(
        "/mcp",
        httplib::Headers{{"Accept", "application/json, text/event-stream"}},
        listed.dump(), "application/json");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    const json result = json::parse(response->body).at("result");
    EXPECT_FALSE(result.contains("_meta"));
    EXPECT_FALSE(result.contains("resultType"));
    EXPECT_FALSE(result.contains("ttlMs"));
    EXPECT_FALSE(result.contains("cacheScope"));
    EXPECT_EQ(result.at("tools").at(0).at("name"), "search_documents");
}

TEST(Mcp, http_rejects_a_bad_request) {
    HttpServer http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";

    json listed = request("tools/list");
    listed["params"] = meta_version();
    httplib::Headers no_version;
    no_version.emplace("Mcp-Method", "tools/list");
    httplib::Client client("127.0.0.1", http.server.http_port());
    const httplib::Result missing = client.Post(
        "/mcp",
        httplib::Headers{{"Accept", "application/json, text/event-stream"},
                         {"Mcp-Method", "tools/list"}},
        listed.dump(), "application/json");
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 400);
    EXPECT_EQ(json::parse(missing->body).at("error").at("code"), -32020);

    listed["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] =
        "2025-06-18";
    const httplib::Result skewed = http.post(listed, {});
    ASSERT_TRUE(skewed);
    EXPECT_EQ(skewed->status, 400);
    EXPECT_EQ(json::parse(skewed->body).at("error").at("code"), -32020);

    json old = request("tools/list");
    old["params"] = meta_version("1999-01-01");
    const httplib::Result unsupported =
        http.post(old, {{"MCP-Protocol-Version", "1999-01-01"}});
    ASSERT_TRUE(unsupported);
    EXPECT_EQ(unsupported->status, 400);
    EXPECT_EQ(json::parse(unsupported->body).at("error").at("code"), -32022);

    json unknown_body = request("tools/unknown");
    unknown_body["params"] = meta_version();
    const httplib::Result unknown = http.post(unknown_body, {});
    ASSERT_TRUE(unknown);
    EXPECT_EQ(unknown->status, 404);
    EXPECT_EQ(json::parse(unknown->body).at("error").at("code"), -32601);

    const json call =
        request("tools/call", {{"name", "search_documents"},
                               {"arguments", json::object()},
                               {"_meta", meta_version().at("_meta")}});
    const httplib::Result name =
        http.post(call, {{"Mcp-Name", "not-the-tool"}});
    ASSERT_TRUE(name);
    EXPECT_EQ(name->status, 400);
    EXPECT_EQ(json::parse(name->body).at("error").at("code"), -32020);

    const httplib::Result note =
        http.post(json{{"jsonrpc", "2.0"},
                       {"method", "notifications/cancelled"},
                       {"params", meta_version()}},
                  {});
    ASSERT_TRUE(note);
    EXPECT_EQ(note->status, 202);
    EXPECT_TRUE(note->body.empty());

    httplib::Client get_client("127.0.0.1", http.server.http_port());
    const httplib::Result got = get_client.Get("/mcp");
    ASSERT_TRUE(got);
    EXPECT_EQ(got->status, 405);

    const httplib::Result foreign =
        http.post(request("tools/list"), {{"Origin", "https://evil.example"}});
    ASSERT_TRUE(foreign);
    EXPECT_EQ(foreign->status, 403);
}

TEST(Mcp, http_matches_a_base64_mcp_name_header) {
    HttpServer http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";

    const json call =
        request("tools/call", {{"name", "search_documents"},
                               {"arguments", json::object()},
                               {"_meta", meta_version().at("_meta")}});

    // The `=?base64?...?=` sentinel is how a name that would not survive an
    // HTTP field travels. Decoding it is what lets the header and body agree.
    const httplib::Result decoded =
        http.post(call, {{"Mcp-Name", "=?base64?c2VhcmNoX2RvY3VtZW50cw==?="}});
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->status, 200);

    // A well-formed sentinel naming something else is still a mismatch.
    const httplib::Result wrong =
        http.post(call, {{"Mcp-Name", "=?base64?bm90LXRoZS10b29s?="}});
    ASSERT_TRUE(wrong);
    EXPECT_EQ(wrong->status, 400);
    EXPECT_EQ(json::parse(wrong->body).at("error").at("code"), -32020);

    // Malformed payloads are a corrupt header, never a value that matches: the
    // decoder rejects what cannot round-trip rather than decoding a prefix.
    for (const char *corrupt : {
             "=?base64?!!!?=",                      // not base64 at all
             "=?base64?c2VhcmNoX2RvY3VtZW50cw?=",   // padding dropped
             "=?base64?c2VhcmNoX2RvY3VtZW50cw==x?=" // trailing junk
         }) {
        const httplib::Result bad = http.post(call, {{"Mcp-Name", corrupt}});
        ASSERT_TRUE(bad) << corrupt;
        EXPECT_EQ(bad->status, 400) << corrupt;
        EXPECT_EQ(json::parse(bad->body).at("error").at("code"), -32020)
            << corrupt;
    }
}

class ContentServer : public mcp::Server<ContentServer> {
public:
    std::string_view server_name() const override { return "content"; }
    std::string_view server_version() const override { return "0"; }

    [[= mcp::tool{}]] mcp::content preview() {
        mcp::content out;
        out.add(mcp::text{"compiled"});
        out.add(mcp::image{"image/png", {0x68, 0x69}});
        return out;
    }
    [[= mcp::tool{}]] mcp::content picture_only() {
        mcp::content out;
        out.add(mcp::image{"image/png", {0x68, 0x69}});
        return out;
    }
    [[= mcp::tool{}]] mcp::content around() {
        mcp::content out;
        out.add(mcp::text{"ab"});
        out.add(mcp::image{"image/png", {0x68, 0x69}});
        out.add(mcp::text{"c"});
        return out;
    }
    [[= mcp::tool{}]] mcp::content blank_image() {
        mcp::content out;
        out.add(mcp::image{"", {}});
        return out;
    }
    [[= mcp::tool{}]] mcp::content empty_content() { return {}; }
    [[= mcp::tool{}]] mcp::content embedded_nul() {
        mcp::content out;
        out.add(mcp::image{"image/png", {0x00, 0x68, 0x69}});
        return out;
    }
    [[= mcp::tool{}]] mcp::content boom_content() {
        throw std::runtime_error("nope");
    }
    [[= mcp::tool{}]] std::string boom_text() {
        throw std::runtime_error("nope");
    }
};

json content_result(ContentServer &_server, std::string_view _name) {
    const json reply = json::parse([&] {
        std::istringstream in{
            request("tools/call",
                    {{"name", _name}, {"arguments", json::object()}})
                .dump() +
            "\n"};
        std::ostringstream out;
        _server.serve(in, out);
        return out.str();
    }());
    return reply.at("result");
}

TEST(McpContent, tools_call_emits_text_then_image) {
    ContentServer server;
    const json result = content_result(server, "preview");
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(result.at("isError"), false);
    EXPECT_EQ(result.at("content").size(), 2u);
    EXPECT_EQ(result.at("content").at(0),
              json({{"type", "text"}, {"text", "compiled"}}));
    EXPECT_EQ(
        result.at("content").at(1),
        json({{"type", "image"}, {"data", "aGk="}, {"mimeType", "image/png"}}));

    const json listed = [&] {
        std::istringstream in{request("tools/list").dump() + "\n"};
        std::ostringstream out;
        ContentServer fresh;
        fresh.serve(in, out);
        return json::parse(out.str()).at("result");
    }();
    const json &preview = listed.at("tools").at(0);
    EXPECT_EQ(preview.at("name"), "preview");
    EXPECT_EQ(preview.at("inputSchema").at("type"), "object");
    EXPECT_FALSE(preview.contains("outputSchema"));
}

TEST(McpContent, call_tool_returns_the_text_only) {
    ContentServer server;
    EXPECT_EQ(server.call_tool("preview", json::object()), "compiled");
    EXPECT_EQ(server.call_tool("picture_only", json::object()), "");
    EXPECT_EQ(server.call_tool("around", json::object()), "abc");
    EXPECT_EQ(server.call_tool("empty_content", json::object()), "");
}

TEST(McpContent, picture_only_is_one_image_block) {
    ContentServer server;
    const json content = content_result(server, "picture_only").at("content");
    ASSERT_EQ(content.size(), 1u);
    EXPECT_EQ(content.at(0).at("type"), "image");
    EXPECT_EQ(content.at(0).at("data"), "aGk=");
}

TEST(McpContent, empty_image_emits_empty_strings) {
    ContentServer server;
    const json block =
        content_result(server, "blank_image").at("content").at(0);
    EXPECT_EQ(block, json({{"type", "image"}, {"data", ""}, {"mimeType", ""}}));
}

TEST(McpContent, empty_content_is_a_successful_empty_array) {
    ContentServer server;
    const json result = content_result(server, "empty_content");
    EXPECT_EQ(result.at("isError"), false);
    EXPECT_EQ(result.at("content"), json::array());
}

TEST(McpContent, embedded_nul_is_encoded) {
    ContentServer server;
    EXPECT_EQ(
        content_result(server, "embedded_nul").at("content").at(0).at("data"),
        "AGhp");
}

TEST(McpContent, throw_is_one_error_text_block) {
    ContentServer server;
    for (const char *name : {"boom_content", "boom_text"}) {
        const json result = content_result(server, name);
        EXPECT_EQ(result.at("isError"), true) << name;
        ASSERT_EQ(result.at("content").size(), 1u) << name;
        EXPECT_EQ(result.at("content").at(0),
                  json({{"type", "text"}, {"text", "nope"}}))
            << name;
    }
}

TEST(McpContent, handshake_era_keeps_the_image_and_drops_result_type) {
    ContentServer server;
    std::string input =
        request("initialize",
                {{"protocolVersion", std::string(mcp::kHandshakeVersion)}})
            .dump() +
        "\n" +
        request("tools/call",
                {{"name", "preview"}, {"arguments", json::object()}})
            .dump() +
        "\n";
    std::istringstream in{input};
    std::ostringstream out;
    server.serve(in, out);
    std::istringstream lines{out.str()};
    std::string line;
    std::getline(lines, line); // initialize
    std::getline(lines, line);
    const json result = json::parse(line).at("result");
    EXPECT_FALSE(result.contains("resultType"));
    EXPECT_FALSE(result.contains("_meta"));
    EXPECT_EQ(result.at("content").at(1).at("data"), "aGk=");
    EXPECT_EQ(result.at("isError"), false);
}
