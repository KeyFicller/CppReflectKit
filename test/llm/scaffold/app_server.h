#pragma once

// The concrete server. Its tools, prompts and resources are ordinary non-static
// member functions carrying a marker; mcp::Server<CppReflectServer>'s constructor
// scans the class, so there is no registration call and no separate namespace.
// The domain types live in app_model.h.
//
// The tools read the repository's own documentation, so the corpus is real files
// rather than a canned response: search_documents scans them, and the resource
// serves one of them verbatim.

#include "app_model.h"
#include "llm/reflection_mcp.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace app {

namespace detail {

/// Root of the corpus the tools read. The build bakes the source directory in so
/// the tools work from any working directory; CPP_REFLECT_CORPUS overrides it to
/// point at a different tree.
inline std::filesystem::path corpus_root()
{
    if (const char* override_path = std::getenv("CPP_REFLECT_CORPUS")) {
        if (*override_path != '\0') {
            return override_path;
        }
    }
#ifdef CPPREF_CORPUS_ROOT
    return CPPREF_CORPUS_ROOT;
#else
    return ".";
#endif
}

/// The corpus files in a stable order: every docs/*.md, then ReadMe.md. Sorting
/// makes ties in the ranking deterministic.
inline std::vector<std::filesystem::path> corpus_files()
{
    const std::filesystem::path root = corpus_root();
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const std::filesystem::path docs = root / "docs";
    if (std::filesystem::is_directory(docs, error)) {
        for (const auto& entry : std::filesystem::directory_iterator(docs, error)) {
            if (entry.is_regular_file() && entry.path().extension() == ".md") {
                files.push_back(entry.path());
            }
        }
    }
    const std::filesystem::path readme = root / "ReadMe.md";
    if (std::filesystem::is_regular_file(readme, error)) {
        files.push_back(readme);
    }
    std::sort(files.begin(), files.end());
    return files;
}

/// The whole file as text, or an error naming it: a resource must fail loudly
/// rather than serve a placeholder.
inline std::string read_file(const std::filesystem::path& _path)
{
    std::ifstream in(_path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("corpus file not readable: " + _path.string());
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string lowered(std::string_view _text)
{
    std::string out(_text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char _c) {
        return static_cast<char>(std::tolower(_c));
    });
    return out;
}

inline std::string trimmed(std::string_view _text)
{
    const auto space = [](unsigned char _c) { return std::isspace(_c) != 0; };
    std::size_t begin = 0;
    std::size_t end = _text.size();
    while (begin < end && space(static_cast<unsigned char>(_text[begin]))) {
        ++begin;
    }
    while (end > begin && space(static_cast<unsigned char>(_text[end - 1]))) {
        --end;
    }
    return std::string(_text.substr(begin, end - begin));
}

/// The document the resource serves, and the scheme its URI is written with. The
/// marker's URI is a literal (an annotation value must be), so the static_assert
/// below the class keeps the two from drifting apart.
inline constexpr std::string_view design_notes_uri = "file:///docs/about_llm.md";
inline constexpr std::string_view file_uri_prefix = "file:///";

/// The path a `file:///` URI names, relative to the corpus root.
inline std::string uri_relative_path(std::string_view _uri)
{
    return std::string(_uri.substr(file_uri_prefix.size()));
}

/// One matching line of the corpus: which file, where, how strong the match, and
/// the line itself.
struct Hit {
    std::string file;
    int line;
    int score;
    std::string text;
};

/// Ranked substring search: one hit per matching line, ranked by how many times
/// the line matches (a line that says it three times outranks one that says it
/// once), ties broken by file then line. `_exact` selects a case-sensitive match;
/// otherwise the search is case-insensitive.
inline std::vector<Hit> search_corpus(std::string_view _query, int _limit, bool _exact)
{
    std::vector<Hit> hits;
    if (_query.empty() || _limit <= 0) {
        return hits;
    }
    const std::filesystem::path root = corpus_root();
    const std::string needle = _exact ? std::string(_query) : lowered(_query);
    for (const std::filesystem::path& path : corpus_files()) {
        std::ifstream in(path);
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const std::string haystack = _exact ? line : lowered(line);
            int count = 0;
            for (std::size_t at = haystack.find(needle); at != std::string::npos;
                 at = haystack.find(needle, at + needle.size())) {
                ++count;
            }
            if (count > 0) {
                const std::filesystem::path rel = path.lexically_relative(root);
                hits.push_back(
                    Hit{rel.empty() ? path.string() : rel.string(), number, count, trimmed(line)});
            }
        }
    }
    std::stable_sort(hits.begin(), hits.end(),
                     [](const Hit& _a, const Hit& _b) { return _a.score > _b.score; });
    if (hits.size() > static_cast<std::size_t>(_limit)) {
        hits.resize(static_cast<std::size_t>(_limit));
    }
    return hits;
}

} // namespace detail

class CppReflectServer : public mcp::Server<CppReflectServer> {
public:
    // A tool: the parameters become a JSON Schema, and the call decodes them.
    [[= mcp::tool{}]]
    [[= js::doc{ .text = js::str("Searches the repository's own documentation.") } ]]
    [[= js::param_docs(js::str("The raw query text."),
                       js::str("Maximum number of hits to return."),
                       js::str("Require an exact (case-sensitive) match.")) ]]
    std::string search_documents(std::string query, int limit, bool exact_match)
    {
        const std::vector<detail::Hit> hits = detail::search_corpus(query, limit, exact_match);
        std::string out = "{\"query\":\"" + js::json_escape(query) + "\"";
        out += ",\"limit\":" + std::to_string(limit);
        out += ",\"exact_match\":" + std::string(exact_match ? "true" : "false");
        out += ",\"hits\":[";
        bool first = true;
        for (const detail::Hit& hit : hits) {
            out += first ? "" : ",";
            first = false;
            out += "{\"file\":\"" + js::json_escape(hit.file) + "\"";
            out += ",\"line\":" + std::to_string(hit.line);
            out += ",\"text\":\"" + js::json_escape(hit.text) + "\"}";
        }
        out += "]}";
        return out;
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
    // still come from reflection. The body serves the file the URI names, so the
    // resource cannot drift from the document it advertises.
    [[= mcp::resource{ .uri = js::str("file:///docs/about_llm.md"),
                       .mime_type = js::str("text/markdown") }]]
    [[= js::doc{ .text = js::str("Design notes for the reflection-driven MCP server.") } ]]
    std::string design_notes()
    {
        return detail::read_file(detail::corpus_root() /
                                 std::filesystem::path(detail::uri_relative_path(
                                     detail::design_notes_uri)));
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

// The marker's URI and the path the body reads must name the same document; a
// second literal that could drift is a compile error instead.
static_assert(mcp::resource_uri<^^CppReflectServer::design_notes>() == detail::design_notes_uri,
              "resource marker URI and detail::design_notes_uri must agree");

} // namespace app
