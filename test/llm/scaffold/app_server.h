#pragma once

// The concrete server. Its tools, prompts and resources are ordinary non-static
// member functions carrying a marker; mcp::Server<CppReflectServer>'s constructor
// scans the class, so there is no registration call and no separate namespace.
// The domain types live in app_model.h.

#include "app_model.h"
#include "mcp.h"

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace app {

class CppReflectServer : public mcp::Server<CppReflectServer> {
public:
    // A tool: the parameters become a JSON Schema, and the call decodes them.
    [[= mcp::tool{}]]
    [[= js::doc{ .text = js::str("Searches the document index.") } ]]
    [[= js::param_docs(js::str("The raw query text."),
                       js::str("Maximum number of hits to return."),
                       js::str("Require an exact match."),
                       js::str("Optional unit filter.")) ]]
    std::string search_documents(std::string query, int limit, bool exact_match,
                                 std::optional<Unit> unit = std::nullopt)
    {
        std::string hits;
        for (int i = 0; i < limit && i < 3; ++i) {
            if (!hits.empty()) {
                hits += ",";
            }
            hits += "{\"rank\":" + std::to_string(i + 1) + ",\"title\":\"" + query + " #" +
                    std::to_string(i + 1) + "\"}";
        }
        const std::string unit_field =
            unit ? (",\"unit\":\"" + js::enum_to_string(*unit) + "\"") : "";
        return "{\"query\":\"" + query + "\",\"limit\":" + std::to_string(limit) +
               ",\"exact_match\":" + (exact_match ? "true" : "false") + unit_field +
               ",\"hits\":[" + hits + "]}";
    }

    // A prompt: the same parameters become PromptArgument descriptors; the body
    // returns the messages JSON that prompts/get hands back.
    [[= mcp::prompt{}]]
    [[= js::doc{ .text = js::str("Explain a reflection concept to a newcomer.") } ]]
    [[= js::param_docs(js::str("The concept to explain, e.g. \"template for\".")) ]]
    std::string explain(std::string topic)
    {
        const nlohmann::json messages = nlohmann::json::array({
            {{"role", "user"},
             {"content", {{"type", "text"}, {"text", "Explain " + topic + " to a newcomer."}}}},
        });
        return messages.dump();
    }

    // A resource: the URI and MIME type live in the marker; name and description
    // still come from reflection. The body returns the resource's text.
    [[= mcp::resource{ .uri = js::str("file:///docs/mcp_automation.md"),
                       .mime_type = js::str("text/markdown") }]]
    [[= js::doc{ .text = js::str("Design notes for the reflection-driven MCP server.") } ]]
    std::string design_notes()
    {
        return "# Reflection-driven MCP\n\nTools, prompts and resources are ordinary C++ "
               "member functions; the server derives the wire descriptors from them.";
    }

protected:
    std::string_view server_name() const override { return "cppreflect"; }
    std::string_view server_version() const override { return "0.1.0"; }
    std::string_view instructions() const override
    {
        return "Reflection-driven tools, prompts and resources for the C++26 "
               "reflection suite.";
    }
};

} // namespace app
