#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
template <class S>
std::vector<json> drive(S &_server, const std::vector<std::string> &_requests) {
    std::string input;
    for (const std::string &request : _requests) {
        input += request;
        input += '\n';
    }
    std::istringstream in{input};
    std::ostringstream out;
    _server.serve(in, out);

    std::vector<json> replies;
    std::istringstream lines{out.str()};
    std::string line;
    while (std::getline(lines, line)) {
        replies.push_back(json::parse(line));
    }
    return replies;
}

std::vector<json> drive(const std::vector<std::string> &_requests) {
    app::CppReflectServer server;
    return drive(server, _requests);
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
    ASSERT_EQ(tools.size(), 2u) << "both marked functions are registered";
    check_eq(tools.at(1).at("name"), "ask_confirmation",
             "declaration order decides the table order");
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
template <class S> struct HttpServerOf {
    S server;
    std::thread thread;

    HttpServerOf() : thread([this] { server.serve_http(0); }) {
        for (int i = 0; i < 100 && server.http_port() < 0; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~HttpServerOf() {
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

using HttpServer = HttpServerOf<app::CppReflectServer>;

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
    EXPECT_THROW(server.call_tool("boom_text", json::object()),
                 std::runtime_error);
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

namespace {

/// Prompts and resources returning more than `std::string`. Its own server, so
/// the scaffold's counts asserted above and in test_contract.py stay put.
class RichServer : public mcp::Server<RichServer> {
public:
    std::string_view server_name() const override { return "rich"; }
    std::string_view server_version() const override { return "0"; }

    [[= mcp::prompt{}]] mcp::message one(std::string topic) {
        return {mcp::role::user, mcp::text{"Explain " + topic}};
    }
    [[= mcp::prompt{}]] std::vector<mcp::message> many(std::string topic) {
        return {{mcp::role::user, mcp::text{"Explain " + topic}},
                {mcp::role::assistant, mcp::image{mcp::mime::png, {0x68, 0x69}}}};
    }
    [[= mcp::prompt{}]] std::string verbatim() {
        return R"([{"role":"assistant","content":{"type":"text","text":"as-is"}}])";
    }
    [[= mcp::prompt{}]] std::string malformed() { return "not json"; }
    [[= mcp::prompt{}]] mcp::message boom() {
        throw std::runtime_error("nope");
    }

    [[= mcp::resource{.uri = js::str("file:///shot.png"),
                      .mime_type = js::str("image/png")}]] mcp::blob shot() {
        return {{0x89, 0x50, 0x4e, 0x47, 0x00, 0x0a}};
    }
    [[= mcp::resource{.uri = js::str("file:///notes.txt"),
                      .mime_type = js::str("text/plain")}]] std::string
    notes() {
        return "plain text";
    }
};

/// A tool return `to_json` can write. The call carries structuredContent;
/// tools/list does not grow an outputSchema.
struct FoundLine {
    std::string file;
    int line;
};

class StructuredServer : public mcp::Server<StructuredServer> {
public:
    std::string_view server_name() const override { return "structured"; }
    std::string_view server_version() const override { return "0"; }

    [[= mcp::tool{}]] FoundLine locate() {
        return {"docs/about_llm.md", 4};
    }
};

/// The reply line for one request through a real serve() loop.
template <class S> json served(S &_server, const json &_request) {
    std::istringstream in{_request.dump() + "\n"};
    std::ostringstream out;
    _server.serve(in, out);
    return json::parse(out.str());
}

} // namespace

TEST(McpRich, a_message_body_is_one_message) {
    RichServer server;
    const json result =
        served(server, request("prompts/get",
                               {{"name", "one"},
                                {"arguments", {{"topic", "P2996"}}}}))
            .at("result");
    ASSERT_EQ(result.at("messages").size(), 1u);
    EXPECT_EQ(result.at("messages").at(0),
              json({{"role", "user"},
                    {"content",
                     {{"type", "text"}, {"text", "Explain P2996"}}}}));
}

TEST(McpRich, a_vector_body_carries_text_and_image) {
    RichServer server;
    const json messages =
        served(server, request("prompts/get",
                               {{"name", "many"}, {"arguments", {{"topic", "T"}}}}))
            .at("result")
            .at("messages");
    ASSERT_EQ(messages.size(), 2u);
    EXPECT_EQ(messages.at(0),
              json({{"role", "user"},
                    {"content", {{"type", "text"}, {"text", "Explain T"}}}}));
    EXPECT_EQ(messages.at(1),
              json({{"role", "assistant"},
                    {"content",
                     {{"type", "image"},
                      {"data", "aGk="},
                      {"mimeType", "image/png"}}}}));
}

TEST(McpRich, the_string_form_is_still_verbatim) {
    RichServer server;
    const json messages =
        served(server, request("prompts/get", {{"name", "verbatim"}}))
            .at("result")
            .at("messages");
    EXPECT_EQ(messages,
              json::parse(R"([{"role":"assistant",)"
                          R"("content":{"type":"text","text":"as-is"}}])"));
}

TEST(McpRich, prompt_error_codes_are_unchanged) {
    RichServer server;
    // Unparsable messages JSON is internal (-32603) ...
    EXPECT_EQ(served(server, request("prompts/get", {{"name", "malformed"}}))
                  .at("error")
                  .at("code"),
              -32603);
    // ... and a throwing body is the author signalling a bad argument (-32602).
    EXPECT_EQ(served(server, request("prompts/get", {{"name", "boom"}}))
                  .at("error")
                  .at("code"),
              -32602);
}

TEST(McpRich, a_blob_has_no_text_key) {
    RichServer server;
    const json contents =
        served(server,
               request("resources/read", {{"uri", "file:///shot.png"}}))
            .at("result")
            .at("contents");
    ASSERT_EQ(contents.size(), 1u);
    EXPECT_EQ(contents.at(0).at("uri"), "file:///shot.png");
    EXPECT_EQ(contents.at(0).at("mimeType"), "image/png");
    // The bytes are the PNG magic plus two, one of them a NUL: base64 survives
    // a payload that could not have gone out as `text`.
    EXPECT_EQ(contents.at(0).at("blob"), "iVBORwAK");
    EXPECT_FALSE(contents.at(0).contains("text"));
}

TEST(McpRich, a_string_resource_is_still_text) {
    RichServer server;
    const json entry =
        served(server,
               request("resources/read", {{"uri", "file:///notes.txt"}}))
            .at("result")
            .at("contents")
            .at(0);
    EXPECT_EQ(entry, json({{"uri", "file:///notes.txt"},
                           {"mimeType", "text/plain"},
                           {"text", "plain text"}}));
    EXPECT_FALSE(entry.contains("blob"));
}

TEST(McpRich, listing_and_read_agree_on_a_binary_mime) {
    RichServer server;
    json shot;
    for (const json &entry :
         served(server, request("resources/list")).at("result").at("resources")) {
        if (entry.at("uri") == "file:///shot.png") {
            shot = entry;
        }
    }
    ASSERT_FALSE(shot.is_null()) << "the binary resource is listed";
    EXPECT_EQ(shot.at("name"), "shot");
    EXPECT_EQ(shot.at("mimeType"), "image/png");
    EXPECT_EQ(served(server,
                     request("resources/read", {{"uri", "file:///shot.png"}}))
                  .at("result")
                  .at("contents")
                  .at(0)
                  .at("mimeType"),
              shot.at("mimeType"));
}

TEST(McpStructured, tools_list_has_no_output_schema) {
    StructuredServer server;
    const json tools =
        served(server, request("tools/list")).at("result").at("tools");
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_EQ(tools.at(0).at("name"), "locate");
    EXPECT_EQ(tools.at(0).at("inputSchema").at("type"), "object");
    EXPECT_FALSE(tools.at(0).contains("outputSchema"));
}

TEST(McpStructured, a_struct_return_is_structured_content) {
    StructuredServer server;
    const json result =
        served(server, request("tools/call", {{"name", "locate"},
                                              {"arguments", json::object()}}))
            .at("result");
    const json value = json{{"file", "docs/about_llm.md"}, {"line", 4}};
    EXPECT_EQ(result.at("isError"), false);
    EXPECT_EQ(result.at("structuredContent"), value);
    EXPECT_EQ(json::parse(result.at("content").at(0).at("text").get<std::string>()),
              value);
    EXPECT_EQ(server.call_tool("locate", json::object()), value.dump());
}

namespace {

/// The form asked for in the elicitation cases.
struct Confirm {
    bool yes;
    std::string note;
};

class ElicitServer : public mcp::Server<ElicitServer> {
public:
    std::string_view server_name() const override { return "elicit"; }
    std::string_view server_version() const override { return "0"; }

    // A struct return goes out as structuredContent, so the test reads the
    // action and the value the way a client does.
    [[= mcp::tool{}]] mcp::elicit_result<Confirm> ask() {
        return elicit_form<Confirm>("确认删除？");
    }
    // Prompt and resource bodies are text only, so this one reports the two
    // actions as words.
    [[= mcp::tool{}]] std::string ask_twice() {
        const std::string first =
            js::enum_to_string(elicit_form<Confirm>("确认删除？").action);
        return first + " " +
               js::enum_to_string(elicit_form<Confirm>("再确认一次？").action);
    }
    [[= mcp::prompt{}]] mcp::message ask_prompt() {
        return {mcp::role::user,
                mcp::text{js::enum_to_string(
                    elicit_form<Confirm>("确认删除？").action)}};
    }
    [[= mcp::resource{.uri = js::str("file:///ask.txt"),
                      .mime_type = js::str("text/plain")}]] std::string
    ask_resource() {
        return js::enum_to_string(elicit_form<Confirm>("确认删除？").action);
    }
};

/// Request `_meta` declaring `_elicitation` as the client's elicitation
/// capability for this request alone.
json caps(json _elicitation) {
    return json{{"io.modelcontextprotocol/clientCapabilities",
                 {{"elicitation", std::move(_elicitation)}}}};
}

json form_caps() { return caps(json{{"form", json::object()}}); }

/// A tools/call of `_tool`, carrying `_answer` as the retry's
/// `inputResponses.elicit` when it is not null.
json call_ask(int _id, json _meta, json _answer = nullptr,
              std::string_view _tool = "ask") {
    json params{{"name", _tool},
                {"arguments", json::object()},
                {"_meta", std::move(_meta)}};
    if (!_answer.is_null()) {
        params["inputResponses"] = {{"elicit", std::move(_answer)}};
    }
    return json{{"jsonrpc", "2.0"},
                {"id", _id},
                {"method", "tools/call"},
                {"params", std::move(params)}};
}

/// The result of one request to a fresh ElicitServer.
json elicit_result_of(const json &_request) {
    ElicitServer server;
    return drive(server, {_request.dump()}).at(0).at("result");
}

json accept(json _content) {
    return json{{"action", "accept"}, {"content", std::move(_content)}};
}

/// The action a structured `complete` tools/call reports, with `value` when
/// the user accepted.
json answer_of(const json &_result) {
    return _result.at("structuredContent");
}

} // namespace

TEST(McpElicit, call_tool_is_unavailable) {
    ElicitServer server;
    EXPECT_EQ(server.call_tool("ask", json::object()),
              json({{"action", "unavailable"}}).dump());
}

TEST(McpElicit, handshake_era_is_unavailable) {
    ElicitServer server;
    const json init = request(
        "initialize", {{"protocolVersion", "2025-11-25"},
                       {"capabilities",
                        {{"elicitation", {{"form", json::object()}}}}}});
    const std::vector<json> replies =
        drive(server, {init.dump(), call_ask(2, form_caps()).dump()});
    ASSERT_EQ(replies.size(), 2u);
    const json &result = replies.at(1).at("result");
    EXPECT_EQ(answer_of(result), json({{"action", "unavailable"}}));
    EXPECT_FALSE(result.contains("inputRequests"));
}

TEST(McpElicit, first_leg_is_input_required) {
    const json result = elicit_result_of(call_ask(1, form_caps()));
    EXPECT_EQ(result.at("resultType"), "input_required");
    EXPECT_EQ(result.at("inputRequests"),
              json({{"elicit",
                     {{"method", "elicitation/create"},
                      {"params",
                       {{"mode", "form"},
                        {"message", "确认删除？"},
                        {"requestedSchema",
                         {{"type", "object"},
                          {"properties",
                           {{"yes", {{"type", "boolean"}}},
                            {"note", {{"type", "string"}}}}},
                          {"required", {"yes", "note"}}}}}}}}}));
    EXPECT_EQ(result.at("_meta").at("io.modelcontextprotocol/serverInfo").at(
                  "name"),
              "elicit");
    EXPECT_FALSE(result.contains("content"));
    EXPECT_FALSE(result.contains("isError"));
}

TEST(McpElicit, null_form_capability_is_unavailable) {
    const json result =
        elicit_result_of(call_ask(1, caps(json{{"form", nullptr}})));
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(answer_of(result), json({{"action", "unavailable"}}));
}

TEST(McpElicit, accept_fills_the_struct) {
    const json result = elicit_result_of(
        call_ask(2, form_caps(), accept({{"yes", true}, {"note", "n"}})));
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(answer_of(result),
              json({{"action", "accept"},
                    {"value", {{"yes", true}, {"note", "n"}}}}));
}

TEST(McpElicit, decline_and_cancel_have_no_value) {
    EXPECT_EQ(answer_of(elicit_result_of(
                  call_ask(2, form_caps(), {{"action", "decline"}}))),
              json({{"action", "decline"}}));
    EXPECT_EQ(answer_of(elicit_result_of(
                  call_ask(2, form_caps(), {{"action", "cancel"}}))),
              json({{"action", "cancel"}}));
}

TEST(McpElicit, missing_form_capability_is_unavailable) {
    const json result = elicit_result_of(call_ask(1, json::object()));
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(answer_of(result), json({{"action", "unavailable"}}));
    EXPECT_FALSE(result.contains("inputRequests"));
}

TEST(McpElicit, capability_does_not_carry_to_the_next_request) {
    ElicitServer server;
    const std::vector<json> replies =
        drive(server, {call_ask(1, form_caps()).dump(),
                       call_ask(2, json::object()).dump()});
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_EQ(replies.at(0).at("result").at("resultType"), "input_required");
    EXPECT_EQ(answer_of(replies.at(1).at("result")),
              json({{"action", "unavailable"}}));
}

TEST(McpElicit, accept_with_missing_field_is_unavailable) {
    EXPECT_EQ(answer_of(elicit_result_of(
                  call_ask(2, form_caps(), accept({{"yes", true}})))),
              json({{"action", "unavailable"}}));
}

TEST(McpElicit, accept_with_non_object_is_unavailable) {
    EXPECT_EQ(answer_of(elicit_result_of(
                  call_ask(2, form_caps(), accept(json::array({true, "n"}))))),
              json({{"action", "unavailable"}}));
}

TEST(McpElicit, responses_without_elicit_key_are_unavailable) {
    json retry = call_ask(2, form_caps());
    retry["params"]["inputResponses"] = {{"other", {{"action", "accept"}}}};
    const json result = elicit_result_of(retry);
    EXPECT_EQ(result.at("resultType"), "complete");
    EXPECT_EQ(answer_of(result), json({{"action", "unavailable"}}));
}

TEST(McpElicit, second_ask_in_one_call_is_unavailable) {
    EXPECT_EQ(elicit_result_of(call_ask(1, form_caps(), nullptr, "ask_twice"))
                  .at("resultType"),
              "input_required");
    const json answer = accept({{"yes", true}, {"note", "n"}});
    EXPECT_EQ(elicit_result_of(call_ask(2, form_caps(), answer, "ask_twice"))
                  .at("content").at(0).at("text"),
              "accept unavailable");
}

TEST(McpElicit, prompt_can_ask) {
    const json result = elicit_result_of(request(
        "prompts/get", {{"name", "ask_prompt"}, {"_meta", form_caps()}}));
    EXPECT_EQ(result.at("resultType"), "input_required");
    EXPECT_TRUE(result.at("inputRequests").contains("elicit"));
}

TEST(McpElicit, resource_can_ask) {
    const json result = elicit_result_of(
        request("resources/read",
                {{"uri", "file:///ask.txt"}, {"_meta", form_caps()}}));
    EXPECT_EQ(result.at("resultType"), "input_required");
    EXPECT_TRUE(result.at("inputRequests").contains("elicit"));
}

TEST(McpElicit, http_can_ask) {
    HttpServerOf<ElicitServer> http;
    ASSERT_GT(http.server.http_port(), 0) << "serve_http did not bind";
    json meta = form_caps();
    meta.update(meta_version().at("_meta"));
    const httplib::Result reply =
        http.post(call_ask(1, std::move(meta)), {{"Mcp-Name", "ask"}});
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->status, 200);
    EXPECT_EQ(json::parse(reply->body).at("result").at("resultType"),
              "input_required");
}

namespace {

class ManualCompletion {
public:
    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> _continuation) {
        std::lock_guard lock(m_mutex);
        if (m_completed) {
            return false;
        }
        m_continuation = _continuation;
        return true;
    }

    void await_resume() const noexcept {}

    bool waiting() const {
        std::lock_guard lock(m_mutex);
        return static_cast<bool>(m_continuation);
    }

    void complete() {
        std::coroutine_handle<> continuation;
        {
            std::lock_guard lock(m_mutex);
            m_completed = true;
            continuation = std::exchange(m_continuation, {});
        }
        if (continuation) {
            continuation.resume();
        }
    }

private:
    mutable std::mutex m_mutex;
    std::coroutine_handle<> m_continuation;
    bool m_completed = false;
};

class LongTaskServer : public mcp::Server<LongTaskServer> {
public:
    std::string_view server_name() const override {
        ++metadata_reads;
        return "tasks";
    }
    std::string_view server_version() const override { return "1"; }

    std::atomic<int> started{0};
    std::atomic<int> cancelled{0};
    mutable std::atomic<int> metadata_reads{0};
    std::atomic<bool> release{false};
    ManualCompletion first_completion;
    ManualCompletion second_completion;
    std::atomic<bool> coroutine_cancelled{false};
    std::promise<std::thread::id> first_resumed_on;

    [[= mcp::tool{.background = true}]] std::string
        slow(std::string_view value) {
        if (value == "fail") {
            throw std::runtime_error("tool failed");
        }
        if (value == "binary") {
            return std::string(1, static_cast<char>(0xff));
        }
        if (value == "large") {
            return std::string(129 * 1024, 'x');
        }
        ++started;
        while (value != "queued" && !release && !task_stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (task_stop_requested()) {
            ++cancelled;
        }
        return std::string(value);
    }

    [[= mcp::tool{}]] mcp::awaitable<std::string>
        wait_for_signal(const std::string &value) {
        co_await std::suspend_never{};
        if (value == "hold" || value == "hold again") {
            co_await await_completion(value == "hold" ? first_completion
                                                      : second_completion);
        }
        if (value == "hold") {
            first_resumed_on.set_value(std::this_thread::get_id());
        }
        co_return value;
    }

    mcp::awaitable<void> await_completion(ManualCompletion &completion) {
        const auto stop = co_await mcp::this_coro::stop_token;
        std::stop_callback cancel(stop, [this, &completion] {
            coroutine_cancelled = true;
            completion.complete();
        });
        co_await completion;
    }
};

constexpr auto kTasksExtension = "io.modelcontextprotocol/tasks";

json long_call(std::string_view _value, bool _negotiate = true,
               std::string_view _tool = "slow") {
    json params = meta_version();
    params["name"] = _tool;
    params["arguments"]["value"] = _value;
    if (_negotiate) {
        params["_meta"]["io.modelcontextprotocol/clientCapabilities"]
              ["extensions"][kTasksExtension] = json::object();
    }
    return request("tools/call", std::move(params));
}

json task_request(std::string_view _method, std::string_view _id) {
    json params = meta_version();
    params["taskId"] = _id;
    params["_meta"]["io.modelcontextprotocol/clientCapabilities"]["extensions"]
          [kTasksExtension] = json::object();
    return request(_method, std::move(params));
}

json post_task_rpc(HttpServerOf<LongTaskServer> &_http, const json &_body,
                   std::string_view _name = {}) {
    httplib::Headers headers;
    if (!_name.empty()) {
        headers.emplace("Mcp-Name", _name);
    }
    const auto response = _http.post(_body, std::move(headers));
    if (!response) {
        throw std::runtime_error("MCP task HTTP request failed");
    }
    return json::parse(response->body);
}

json wait_for_task(HttpServerOf<LongTaskServer> &_http,
                   const std::string &_id) {
    json task;
    for (int i = 0; i < 100; ++i) {
        task = post_task_rpc(_http, task_request("tasks/get", _id), _id)
                   .at("result");
        if (task.at("status") != "working") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return task;
}

bool wait_until(const auto &_condition) {
    for (int i = 0; i < 100; ++i) {
        if (_condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

TEST(Mcp, stdio_tasks_are_negotiated_per_call) {
    LongTaskServer server;
    const auto replies = drive(
        server, {request("server/discover").dump(), long_call("fail").dump(),
                 long_call("fail", false).dump(), request("ping").dump()});
    EXPECT_EQ(replies.at(0)
                  .at("result")
                  .at("capabilities")
                  .at("extensions")
                  .at(kTasksExtension),
              json::object());
    EXPECT_EQ(replies.at(1).at("result").at("resultType"), "task");
    EXPECT_EQ(replies.at(2).at("result").at("resultType"), "complete");
    EXPECT_TRUE(replies.at(2).at("result").at("isError").get<bool>());
    EXPECT_TRUE(replies.at(3).contains("result"));

    json legacy_call = long_call("fail");
    legacy_call["params"]["_meta"].erase(
        "io.modelcontextprotocol/protocolVersion");
    const auto legacy =
        drive(server, {request("initialize").dump(), legacy_call.dump(),
                       request("tasks/get", {{"taskId", "unknown"}}).dump()});
    EXPECT_FALSE(
        legacy.at(0).at("result").at("capabilities").contains("extensions"));
    EXPECT_TRUE(legacy.at(1).at("result").contains("content"));
    EXPECT_EQ(legacy.at(2).at("error").at("code"), -32601);
}

TEST(Mcp, non_object_tool_arguments_are_rejected_before_execution) {
    const auto invalid_inline = request(
        "tools/call",
        {{"name", "search_documents"}, {"arguments", json::array()}});
    const json inline_reply = drive({invalid_inline.dump()}).at(0);
    ASSERT_TRUE(inline_reply.contains("error")) << inline_reply.dump();
    EXPECT_EQ(inline_reply.at("error").at("code"), -32602);

    LongTaskServer server;
    auto invalid_background = long_call("queued");
    invalid_background["params"]["arguments"] = json::array();
    auto invalid_coroutine = long_call("queued", true, "wait_for_signal");
    invalid_coroutine["params"]["arguments"] = json::array();
    const auto replies =
        drive(server, {invalid_background.dump(), invalid_coroutine.dump()});
    for (const json &reply : replies) {
        ASSERT_TRUE(reply.contains("error")) << reply.dump();
        EXPECT_EQ(reply.at("error").at("code"), -32602);
    }
    EXPECT_EQ(server.started.load(), 0);
}

TEST(Mcp, http_tasks_wait_for_workers_and_allow_polling_and_cancellation) {
    HttpServerOf<LongTaskServer> http;
    const json first =
        post_task_rpc(http, long_call("first"), "slow").at("result");
    ASSERT_EQ(first.at("resultType"), "task");
    const auto first_id = first.at("taskId").get<std::string>();
    const auto second_id = post_task_rpc(http, long_call("second"), "slow")
                               .at("result")
                               .at("taskId")
                               .get<std::string>();
    ASSERT_TRUE(wait_until([&] { return http.server.started == 2; }));
    const auto queued_id = post_task_rpc(http, long_call("queued"), "slow")
                               .at("result")
                               .at("taskId")
                               .get<std::string>();
    const auto cancelled_id = post_task_rpc(http, long_call("queued"), "slow")
                                  .at("result")
                                  .at("taskId")
                                  .get<std::string>();

    EXPECT_TRUE(post_task_rpc(http, request("ping", meta_version()))
                    .contains("result"));
    const json poll = task_request("tasks/get", queued_id);
    EXPECT_TRUE(post_task_rpc(http, poll, first_id).contains("error"));
    EXPECT_TRUE(post_task_rpc(http, poll).contains("error"));
    EXPECT_EQ(post_task_rpc(http, poll, queued_id).at("result").at("status"),
              "working");
    EXPECT_EQ(post_task_rpc(http, task_request("tasks/cancel", cancelled_id),
                            cancelled_id)
                  .at("result")
                  .at("resultType"),
              "complete");
    EXPECT_EQ(wait_for_task(http, cancelled_id).at("status"), "cancelled");

    EXPECT_TRUE(
        post_task_rpc(http, task_request("tasks/cancel", first_id), first_id)
            .contains("result"));
    EXPECT_EQ(wait_for_task(http, first_id).at("status"), "cancelled");
    const json queued = wait_for_task(http, queued_id);
    ASSERT_EQ(queued.at("status"), "completed");
    EXPECT_EQ(queued.at("result").at("content").at(0).at("text"), "queued");
    EXPECT_EQ(http.server.started.load(), 3);
    EXPECT_EQ(http.server.cancelled.load(), 1);

    http.server.release = true;
    EXPECT_EQ(wait_for_task(http, second_id).at("status"), "completed");
    const auto failed_id = post_task_rpc(http, long_call("fail"), "slow")
                               .at("result")
                               .at("taskId")
                               .get<std::string>();
    const json tool_error = wait_for_task(http, failed_id);
    ASSERT_EQ(tool_error.at("status"), "completed");
    EXPECT_TRUE(tool_error.at("result").at("isError").get<bool>());
}

TEST(Mcp, coroutines_share_the_pool_and_release_workers_when_suspended) {
    HttpServerOf<LongTaskServer> http;
    EXPECT_EQ(http.server.call_tool("wait_for_signal", {{"value", "cli"}}),
              "cli");
    const auto running = post_task_rpc(http, long_call("first"), "slow")
                             .at("result")
                             .at("taskId")
                             .get<std::string>();
    post_task_rpc(http, long_call("second"), "slow").at("result").at("taskId");
    ASSERT_TRUE(wait_until([&] { return http.server.started == 2; }));

    const auto submit = [&](std::string_view value) {
        return post_task_rpc(http, long_call(value, true, "wait_for_signal"),
                             "wait_for_signal")
            .at("result")
            .at("taskId")
            .get<std::string>();
    };
    const auto first = submit("hold");
    const auto second = submit("hold again");
    EXPECT_FALSE(http.server.first_completion.waiting());
    EXPECT_FALSE(http.server.second_completion.waiting());
    EXPECT_EQ(post_task_rpc(http, task_request("tasks/get", second), second)
                  .at("result")
                  .at("status"),
              "working");

    EXPECT_TRUE(
        post_task_rpc(http, task_request("tasks/cancel", running), running)
            .contains("result"));
    ASSERT_TRUE(wait_until([&] {
        return http.server.first_completion.waiting() &&
               http.server.second_completion.waiting();
    }));
    auto resumed_on = http.server.first_resumed_on.get_future();
    http.server.first_completion.complete();
    ASSERT_EQ(resumed_on.wait_for(std::chrono::seconds(1)),
              std::future_status::ready);
    EXPECT_EQ(resumed_on.get(), std::this_thread::get_id());
    EXPECT_EQ(
        wait_for_task(http, first).at("result").at("content").at(0).at("text"),
        "hold");
    EXPECT_TRUE(
        post_task_rpc(http, task_request("tasks/cancel", second), second)
            .contains("result"));
    EXPECT_EQ(wait_for_task(http, second).at("status"), "cancelled");
    EXPECT_TRUE(http.server.coroutine_cancelled.load());

    const auto direct = post_task_rpc(
        http, long_call("sync fallback", false, "wait_for_signal"),
        "wait_for_signal");
    EXPECT_EQ(direct.at("result").at("content").at(0).at("text"),
              "sync fallback");
}

TEST(Mcp, task_runtime_is_isolated_and_cancels_coroutines_on_shutdown) {
    HttpServerOf<LongTaskServer> first;
    HttpServerOf<LongTaskServer> second;
    const auto id =
        post_task_rpc(first, long_call("hold", true, "wait_for_signal"),
                      "wait_for_signal")
            .at("result")
            .at("taskId")
            .get<std::string>();
    ASSERT_TRUE(
        wait_until([&] { return first.server.first_completion.waiting(); }));
    EXPECT_EQ(post_task_rpc(second, task_request("tasks/get", id), id)
                  .at("error")
                  .at("code"),
              -32602);

    first.server.stop_http();
    first.thread.join();
    EXPECT_TRUE(first.server.coroutine_cancelled.load());
    EXPECT_TRUE(post_task_rpc(second, request("ping", meta_version()))
                    .contains("result"));
}

TEST(Mcp, stopping_http_releases_a_synchronous_coroutine_reply) {
    HttpServerOf<LongTaskServer> http;
    auto direct = std::async(std::launch::async, [&] {
        return post_task_rpc(http, long_call("hold", false, "wait_for_signal"),
                             "wait_for_signal");
    });
    ASSERT_TRUE(
        wait_until([&] { return http.server.first_completion.waiting(); }));
    http.server.stop_http();
    EXPECT_EQ(direct.get().at("error").at("code"), -32000);
    http.thread.join();
    EXPECT_TRUE(http.server.coroutine_cancelled.load());
}

TEST(Mcp, task_operations_require_the_extension_on_each_request) {
    HttpServerOf<LongTaskServer> http;
    const auto id =
        post_task_rpc(http, long_call("ready", true, "wait_for_signal"),
                      "wait_for_signal")
            .at("result")
            .at("taskId")
            .get<std::string>();
    for (const auto *method : {"tasks/get", "tasks/cancel", "tasks/update"}) {
        json body = task_request(method, id);
        body["params"]["_meta"].erase(
            "io.modelcontextprotocol/clientCapabilities");
        const json reply = post_task_rpc(http, body, id);
        ASSERT_TRUE(reply.contains("error")) << method;
        EXPECT_EQ(reply.at("error").at("code"), -32021) << method;
    }
    json update = task_request("tasks/update", id);
    update["params"]["inputResponses"] = json::object();
    EXPECT_EQ(post_task_rpc(http, update, id).at("result").at("resultType"),
              "complete");
    EXPECT_EQ(wait_for_task(http, id).at("status"), "completed");
}

TEST(Mcp, marked_cpu_tools_use_the_pool_even_without_tasks_capability) {
    HttpServerOf<LongTaskServer> http;
    const auto first = post_task_rpc(http, long_call("first"), "slow")
                           .at("result")
                           .at("taskId")
                           .get<std::string>();
    post_task_rpc(http, long_call("second"), "slow").at("result").at("taskId");
    ASSERT_TRUE(wait_until([&] { return http.server.started == 2; }));

    auto direct = std::async(std::launch::async, [&] {
        return post_task_rpc(http, long_call("queued", false), "slow");
    });
    EXPECT_EQ(direct.wait_for(std::chrono::milliseconds(60)),
              std::future_status::timeout);
    EXPECT_TRUE(post_task_rpc(http, request("ping", meta_version()))
                    .contains("result"));
    EXPECT_TRUE(post_task_rpc(http, task_request("tasks/cancel", first), first)
                    .contains("result"));
    EXPECT_EQ(direct.get().at("result").at("content").at(0).at("text"),
              "queued");
}

TEST(Mcp, task_admission_waits_and_keeps_control_requests_responsive) {
    HttpServerOf<LongTaskServer> http;
    const auto first = post_task_rpc(http, long_call("first"), "slow")
                           .at("result")
                           .at("taskId")
                           .get<std::string>();
    post_task_rpc(http, long_call("second"), "slow").at("result").at("taskId");
    ASSERT_TRUE(wait_until([&] { return http.server.started == 2; }));
    for (int i = 0; i < 30; ++i) {
        ASSERT_EQ(post_task_rpc(http, long_call("queued"), "slow")
                      .at("result")
                      .at("resultType"),
                  "task");
    }

    const auto overload = http.post(long_call("ready", true, "wait_for_signal"),
                                    {{"Mcp-Name", "wait_for_signal"}});
    ASSERT_TRUE(overload);
    EXPECT_EQ(overload->status, 503);
    EXPECT_EQ(overload->get_header_value("Retry-After"), "1");

    auto waiting = std::async(std::launch::async, [&] {
        return post_task_rpc(http, long_call("queued"), "slow");
    });
    EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(60)),
              std::future_status::timeout);
    EXPECT_TRUE(post_task_rpc(http, request("ping", meta_version()))
                    .contains("result"));
    EXPECT_TRUE(post_task_rpc(http, task_request("tasks/cancel", first), first)
                    .contains("result"));
    EXPECT_EQ(waiting.get().at("result").at("resultType"), "task");
    EXPECT_EQ(http.server.metadata_reads.load(), 1);
}

TEST(Mcp, invalid_and_oversized_task_results_fail_in_the_task_record) {
    HttpServerOf<LongTaskServer> http;
    for (const auto *value : {"binary", "large"}) {
        const auto id = post_task_rpc(http, long_call(value), "slow")
                            .at("result")
                            .at("taskId")
                            .get<std::string>();
        const json result = wait_for_task(http, id);
        EXPECT_EQ(result.at("status"), "failed") << value;
        EXPECT_EQ(result.at("error").at("code"), -32603) << value;
    }
}

TEST(Mcp, stopping_http_releases_a_synchronous_response_waiting_for_a_worker) {
    HttpServerOf<LongTaskServer> http;
    post_task_rpc(http, long_call("first"), "slow").at("result").at("taskId");
    post_task_rpc(http, long_call("second"), "slow").at("result").at("taskId");
    ASSERT_TRUE(wait_until([&] { return http.server.started == 2; }));

    auto direct = std::async(std::launch::async, [&] {
        return post_task_rpc(http, long_call("queued", false), "slow");
    });
    EXPECT_EQ(direct.wait_for(std::chrono::milliseconds(60)),
              std::future_status::timeout);
    http.server.stop_http();
    EXPECT_EQ(direct.get().at("error").at("code"), -32000);
    http.thread.join();
    EXPECT_EQ(http.server.started.load(), 2);
    EXPECT_EQ(http.server.cancelled.load(), 2);
}

TEST(Mcp, stdio_reaches_eof_when_a_synchronous_call_cannot_get_a_worker) {
    LongTaskServer server;
    const auto replies = drive(
        server, {long_call("first").dump(), long_call("second").dump(),
                 long_call("queued", false).dump(), request("ping").dump()});
    EXPECT_EQ(replies.at(0).at("result").at("resultType"), "task");
    EXPECT_EQ(replies.at(1).at("result").at("resultType"), "task");
    EXPECT_EQ(replies.at(2).at("error").at("code"), -32000);
    EXPECT_TRUE(replies.at(3).contains("result"));
    EXPECT_EQ(server.cancelled.load(), 2);
}
