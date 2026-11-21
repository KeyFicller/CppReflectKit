#pragma once

#include "../reflection_json.h"
#include "mcp_awaitable.h"

#include <cppcodec/base64_rfc4648.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mcp {

/**
 * @brief Exposes a member function as a prompt.
 */
struct prompt {};

/**
 * @brief Exposes a member function as a tool with client-facing hints.
 * Destructive and idempotent hints apply only to tools that can write.
 */
struct tool {
    bool read_only = false;
    bool destructive = true;
    bool idempotent = false;
    bool open_world = true;
    /**
     * @brief Run this synchronous tool on the task pool.
     */
    bool background = false;
};

/**
 * @brief Exposes a resource by URI with an optional MIME type.
 */
template <std::size_t U, std::size_t M = 1> struct resource {
    js::str<U> uri;
    js::str<M> mime_type;
};

/**
 * @brief Deduces resource annotation storage from a URI and MIME type.
 */
template <std::size_t U, std::size_t M>
resource(js::str<U>, js::str<M>) -> resource<U, M>;
/**
 * @brief Deduces a resource annotation with an empty MIME type.
 */
template <std::size_t U> resource(js::str<U>) -> resource<U, 1>;

struct text {
    std::string value;
};

/**
 * @brief Raw image bytes encoded as base64 for transport.
 */
struct image {
    std::string mime_type;
    std::vector<std::uint8_t> bytes;
};

namespace mime {
inline constexpr const char *png = "image/png";
inline constexpr const char *jpeg = "image/jpeg";
inline constexpr const char *gif = "image/gif";
inline constexpr const char *webp = "image/webp";
}

/**
 * @brief An ordered sequence of text and image blocks returned by a tool.
 */
struct content {
    std::vector<std::variant<text, image>> blocks;
    /**
     * @brief Appends a text block to the result.
     * @param _block The text block to append.
     */
    void add(text _block) { blocks.push_back(std::move(_block)); }
    /**
     * @brief Appends an image block to the result.
     * @param _block The image block to append.
     */
    void add(image _block) { blocks.push_back(std::move(_block)); }
};

enum class role { user, assistant };

/**
 * @brief A prompt message holds one content block.
 */
struct message {
    role role;
    std::variant<text, image> block;
};

/**
 * @brief Binary resource payload. The resource annotation supplies the MIME
 * type.
 */
struct blob {
    std::vector<std::uint8_t> bytes;
};

/**
 * @brief Outcome of elicitation. Unavailable includes invalid or missing
 * replies.
 */
enum class elicit_action { accept, decline, cancel, unavailable };

template <typename T> struct elicit_result {
    elicit_action action;
    /**
     * @brief Populated when the action is accept.
     */
    std::optional<T> value;
};

namespace detail {

using cppreflect::detail::find_annotation;
using cppreflect::detail::has_annotation;
using cppreflect::detail::static_text;

template <std::meta::info Fn> consteval std::string_view tool_description() {
    return static_text(js::detail::description_of<Fn>());
}

template <std::meta::info Fn> consteval tool tool_marker() {
    constexpr std::meta::info anno = find_annotation<^^tool>(Fn);
    static_assert(anno != std::meta::info{},
                  "mcp tool needs [[= mcp::tool{}]]");
    return std::meta::extract<tool>(anno);
}

template <std::meta::info Fn> consteval auto resource_marker() {
    constexpr std::meta::info anno = find_annotation<^^mcp::resource>(Fn);
    static_assert(anno != std::meta::info{},
                  "mcp resource needs [[= mcp::resource{...}]]");
    return std::meta::extract<typename[:std::meta::type_of(anno):]>(anno);
}

template <typename T> struct is_prompt_argument : std::false_type {};
template <> struct is_prompt_argument<std::string> : std::true_type {};
template <>
struct is_prompt_argument<std::optional<std::string>> : std::true_type {};

template <std::meta::info Fn, std::size_t I>
consteval std::string prompt_argument_json() {
    using PT = js::detail::param_type_at<Fn, I>;
    static_assert(is_prompt_argument<PT>::value,
                  "mcp prompt argument must be std::string or "
                  "std::optional<std::string>");
    const std::string name(
        std::meta::identifier_of(js::detail::param_at<Fn, I>()));
    const std::string desc = js::detail::param_description<Fn, I>();
    std::string out = "{\"name\":\"" + js::detail::json_escape(name) + "\"";
    if (!desc.empty()) {
        out += ",\"description\":\"" + js::detail::json_escape(desc) + "\"";
    }
    const bool required = !js::detail::is_optional<PT>::value;
    out += std::string(",\"required\":") + (required ? "true" : "false") + "}";
    return out;
}

template <std::meta::info Fn, std::size_t... Is>
consteval std::string prompt_arguments_impl(std::index_sequence<Is...>) {
    std::string out = "[";
    std::size_t written = 0;
    ((void)([&] {
         if (written++ != 0) {
             out += ",";
         }
         out += prompt_argument_json<Fn, Is>();
     }()),
     ...);
    out += "]";
    return out;
}

template <std::meta::info Fn> consteval std::string_view prompt_arguments() {
    return static_text(prompt_arguments_impl<Fn>(
        std::make_index_sequence<js::detail::param_count<Fn>()>()));
}

inline std::string base64_of(const std::vector<std::uint8_t> &_bytes) {
    return cppcodec::base64_rfc4648::encode<std::string>(_bytes.data(),
                                                         _bytes.size());
}

inline nlohmann::json encode_block(const std::variant<text, image> &_block) {
    using nlohmann::json;
    if (const auto *written = std::get_if<text>(&_block)) {
        return json{{"type", "text"}, {"text", written->value}};
    }
    const auto &picture = std::get<image>(_block);
    return json{{"type", "image"},
                {"data", base64_of(picture.bytes)},
                {"mimeType", picture.mime_type}};
}

inline nlohmann::json encode_message(const message &_message) {
    return nlohmann::json{{"role", js::enum_to_string(_message.role)},
                          {"content", encode_block(_message.block)}};
}

inline nlohmann::json text_content(std::string _text, bool _is_error) {
    using nlohmann::json;
    return json{{"resultType", "complete"},
                {"content", json::array({json{{"type", "text"},
                                              {"text", std::move(_text)}}})},
                {"isError", _is_error}};
}

inline nlohmann::json encode_content_result(const content &_content) {
    using nlohmann::json;
    json blocks = json::array();
    for (const auto &block : _content.blocks) {
        blocks.push_back(encode_block(block));
    }
    return json{{"resultType", "complete"},
                {"content", std::move(blocks)},
                {"isError", false}};
}

template <typename R> consteval bool structured_tool_return() {
    if constexpr (std::is_same_v<R, std::string> ||
                  std::is_same_v<R, content> || std::is_same_v<R, text> ||
                  std::is_same_v<R, image> || std::is_same_v<R, message> ||
                  std::is_same_v<R, blob>) {
        return false;
    } else {
        return js::detail::can_schema<R>();
    }
}

template <typename T> nlohmann::json encode_tool_result(const T &_value) {
    if constexpr (std::is_same_v<T, std::string>) {
        return text_content(_value, false);
    } else if constexpr (std::is_same_v<T, content>) {
        return encode_content_result(_value);
    } else {
        static_assert(
            structured_tool_return<T>(),
            "mcp tool must return std::string, mcp::content, or a type "
            "with a JSON schema");
        auto text = js::to_json(_value);
        auto structured = nlohmann::json::parse(text);
        auto result = text_content(std::move(text), false);
        result["structuredContent"] = std::move(structured);
        return result;
    }
}

template <typename T> inline constexpr bool is_awaitable = false;
template <typename T> inline constexpr bool is_awaitable<awaitable<T>> = true;

template <std::meta::info Fn, typename Self>
using tool_return =
    std::remove_cvref_t<decltype(js::invoke_member_with_json<Fn>(
        std::declval<Self &>(), std::declval<const nlohmann::json &>()))>;

template <std::meta::info Fn, typename Self>
awaitable<nlohmann::json> invoke_coroutine_tool(void *_self,
                                                const nlohmann::json &_args) {
    auto args = js::detail::bind_arguments<Fn>(
        _args, std::make_index_sequence<js::detail::param_count<Fn>()>());
    auto work = std::apply(
        [_self](auto &...values) {
            return (static_cast<Self *>(_self)->*&[:Fn:])(values...);
        },
        args);
    co_return encode_tool_result(co_await std::move(work));
}

using CoroutineTool = awaitable<nlohmann::json> (*)(void *,
                                                    const nlohmann::json &);

template <std::meta::info Fn, typename Self>
constexpr CoroutineTool coroutine_tool() {
    if constexpr (is_awaitable<tool_return<Fn, Self>>) {
        return &invoke_coroutine_tool<Fn, Self>;
    }
    return nullptr;
}

inline nlohmann::json wait_for_coroutine_tool(awaitable<nlohmann::json> _work) {
    std::promise<nlohmann::json> reply;
    auto result = reply.get_future();
    start_coroutine(std::move(_work), {},
                    [reply = std::move(reply)](std::exception_ptr _error,
                                               nlohmann::json _result) mutable {
                        if (_error) {
                            reply.set_exception(_error);
                        } else {
                            reply.set_value(std::move(_result));
                        }
                    });
    return result.get();
}

inline awaitable<nlohmann::json>
run_coroutine_task(CoroutineTool _tool, void *_self, nlohmann::json _args) {
    try {
        co_return co_await _tool(_self, _args);
    } catch (const std::exception &_e) {
        co_return text_content(_e.what(), true);
    }
}

template <std::meta::info Fn, typename Self>
nlohmann::json invoke_tool_entry(void *_self, const nlohmann::json &_args) {
    if constexpr (is_awaitable<tool_return<Fn, Self>>) {
        return wait_for_coroutine_tool(
            invoke_coroutine_tool<Fn, Self>(_self, _args));
    } else {
        return encode_tool_result(js::invoke_member_with_json<Fn>(
            *static_cast<Self *>(_self), _args));
    }
}

template <std::meta::info Fn, typename Self>
nlohmann::json invoke_prompt_entry(void *_self, const nlohmann::json &_args) {
    using nlohmann::json;
    using R = std::remove_cvref_t<decltype(js::invoke_member_with_json<Fn>(
        *static_cast<Self *>(_self), _args))>;
    static_assert(std::is_same_v<R, std::string> ||
                      std::is_same_v<R, message> ||
                      std::is_same_v<R, std::vector<message>>,
                  "mcp prompt must return std::string, mcp::message or "
                  "std::vector<mcp::message>");
    if constexpr (std::is_same_v<R, std::string>) {
        return json::parse(js::invoke_member_with_json<Fn>(
            *static_cast<Self *>(_self), _args));
    } else if constexpr (std::is_same_v<R, message>) {
        return json::array({encode_message(js::invoke_member_with_json<Fn>(
            *static_cast<Self *>(_self), _args))});
    } else {
        json messages = json::array();
        for (const message &one : js::invoke_member_with_json<Fn>(
                 *static_cast<Self *>(_self), _args)) {
            messages.push_back(encode_message(one));
        }
        return messages;
    }
}

template <std::meta::info Fn, typename Self>
nlohmann::json invoke_resource_entry(void *_self, const nlohmann::json &_args) {
    using R = std::remove_cvref_t<decltype(js::invoke_member_with_json<Fn>(
        *static_cast<Self *>(_self), _args))>;
    static_assert(std::is_same_v<R, std::string> || std::is_same_v<R, blob>,
                  "mcp resource must return std::string or mcp::blob");
    if constexpr (std::is_same_v<R, std::string>) {
        return nlohmann::json{
            {"text", js::invoke_member_with_json<Fn>(
                         *static_cast<Self *>(_self), _args)}};
    } else {
        const blob payload =
            js::invoke_member_with_json<Fn>(*static_cast<Self *>(_self), _args);
        return nlohmann::json{{"blob", base64_of(payload.bytes)}};
    }
}

}

}
