#pragma once

// The reflection-driven JSON layer: schema generation (struct/function -> JSON
// Schema), the annotation types (doc / desc / param_docs), and the reverse
// decoder (JSON -> C++). Lifted verbatim out of test_json_schema.cpp so the MCP
// server (mcp.h) can reuse it without a translation-unit dependency.

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <meta>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace meta = std::meta;

namespace js {

// --- annotation value types -------------------------------------------------
// An annotation value must be structural, so a string cannot be a string_view.
// str<N> holds exactly N characters, including the NUL, e.g. str<6>{"hello"}.
template <std::size_t N> struct str {
    char data[N]{};
    constexpr str() = default;
    constexpr str(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            data[i] = s[i];
        }
    }
};
template <std::size_t N> str(const char (&)[N]) -> str<N>;

// Function-level documentation, applied as [[= js::doc{ .text = js::str("...")
// } ]].
template <std::size_t N = 1> struct doc {
    str<N> text;
};

// Field-level description for a struct member, applied directly on the member
// as
// [[= js::desc{ .text = js::str("...") } ]]. The same type is reused for the
// positional parameter list below.
template <std::size_t N = 1> struct desc {
    str<N> text;
};

// A structural, variadic list of descriptions. std::tuple is not a structural
// type in this implementation, so we roll our own. A constructor keeps the
// nested aggregate from tripping -Wmissing-braces/-Wmissing-field-initializers.
template <typename... Ts> struct doc_list;
template <> struct doc_list<> {};
template <typename T, typename... Ts> struct doc_list<T, Ts...> {
    T head;
    doc_list<Ts...> tail;

    constexpr doc_list() = default;
    constexpr doc_list(T _head, Ts... _tail) : head(_head), tail(_tail...) {}
};

/// Function-parameter descriptions, matched to parameters by position:
///   [[= js::param_docs(js::str("first"), js::str("second")) ]]
/// (An annotation on the parameter itself is not readable: annotations_of() on
/// a parameter reflection fails, so the metadata lives on the function
/// instead.)
consteval auto param_docs(auto... _xs) {
    return doc_list<decltype(_xs)...>{_xs...};
}

// --- type traits ------------------------------------------------------------

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};

template <typename T> struct is_sequence : std::false_type {};
template <typename T, typename A>
struct is_sequence<std::vector<T, A>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = false;
};
template <typename T, std::size_t N>
struct is_sequence<std::array<T, N>> : std::true_type {
    using value_type = T;
    static constexpr bool fixed_size = true;
    static constexpr std::size_t extent = N;
};

template <typename T> struct is_variant : std::false_type {};
template <typename... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};

template <typename T>
inline constexpr bool is_string_like_v =
    std::is_same_v<std::remove_cv_t<T>, std::string> ||
    std::is_same_v<std::remove_cv_t<T>, std::string_view>;

/// The scalar types json_type_name() can name: everything param_schema() does
/// not route to optional/sequence/enum/nested-object. Anything else is
/// unsupported.
template <typename T>
inline constexpr bool is_json_scalar_v =
    std::is_same_v<std::remove_cv_t<T>, bool> || std::is_integral_v<T> ||
    std::is_floating_point_v<T> || is_string_like_v<T>;

// --- JSON helpers -----------------------------------------------------------

constexpr std::string json_escape(std::string_view _s) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (char c : _s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            // Every other control character must be escaped as \u00XX, or the
            // output is not valid JSON.
            if (u < 0x20) {
                out += "\\u00";
                out += kHex[(u >> 4) & 0xF];
                out += kHex[u & 0xF];
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

/// Decimal text for a non-negative integer, usable at consteval (std::to_string
/// is not constexpr here).
consteval std::string decimal(std::size_t _v) {
    if (_v == 0) {
        return "0";
    }
    char buf[24];
    std::size_t len = 0;
    while (_v > 0) {
        buf[len++] = static_cast<char>('0' + _v % 10);
        _v /= 10;
    }
    std::string out;
    while (len > 0) {
        out += buf[--len];
    }
    return out;
}

/// JSON type keyword for a reflected type.
consteval const char *json_type_name(meta::info _t) {
    _t = meta::dealias(_t);
    // bool must be tested before is_integral_type, which is also true for bool.
    if (meta::is_same_type(_t, ^^bool)) {
        return "boolean";
    }
    if (meta::is_integral_type(_t)) {
        return "integer";
    }
    if (meta::is_floating_point_type(_t)) {
        return "number";
    }
    if (meta::is_same_type(_t, ^^std::string) ||
        meta::is_same_type(_t, ^^std::string_view)) {
        return "string";
    }
    // Unreachable: param_schema() only calls this for types satisfying
    // is_json_scalar_v, and the remaining scalars are the strings above.
    return "string";
}

// --- description annotations ------------------------------------------------

/// First annotation on `_e` whose type is an instantiation of the template
/// `Tmpl`, or an empty reflection when there is none.
template <meta::info Tmpl> consteval meta::info find_annotation(meta::info _e) {
    for (meta::info a : meta::annotations_of(_e)) {
        const meta::info t = meta::remove_const(meta::type_of(a));
        if (meta::has_template_arguments(t) && meta::template_of(t) == Tmpl) {
            return a;
        }
    }
    return {};
}

/// Text held by the first `Tmpl` annotation on `E`, or "" when there is none.
template <meta::info Tmpl, meta::info E> consteval std::string text_of() {
    if constexpr (find_annotation<Tmpl>(E) == meta::info{}) {
        return {};
    } else {
        constexpr auto cfg =
            meta::extract<typename[:meta::type_of(find_annotation<Tmpl>(E)):]>(
                find_annotation<Tmpl>(E));
        return std::string(std::string_view(cfg.text.data));
    }
}

template <meta::info Fn> consteval std::size_t param_doc_count() {
    if (find_annotation<^^doc_list>(Fn) == meta::info{}) {
        return 0;
    }
    return meta::template_arguments_of(meta::remove_const(meta::type_of(
                                           find_annotation<^^doc_list>(Fn))))
        .size();
}

template <std::size_t I, typename List> consteval auto nth_doc(List _list) {
    if constexpr (I == 0) {
        return _list.head;
    } else {
        return nth_doc<I - 1>(_list.tail);
    }
}

/// Positional description for parameter `I` of `Fn`, or "" if undocumented.
template <meta::info Fn, std::size_t I>
consteval std::string param_description() {
    if constexpr (find_annotation<^^doc_list>(Fn) == meta::info{}) {
        return {};
    } else if constexpr (I < param_doc_count<Fn>()) {
        constexpr auto list = meta::extract<
            typename[:meta::type_of(find_annotation<^^doc_list>(Fn)):]>(
            find_annotation<^^doc_list>(Fn));
        return std::string(std::string_view(nth_doc<I>(list).data));
    } else {
        return {};
    }
}

/// Splice a description into a generated schema, which is always one JSON
/// object.
consteval std::string with_description(std::string _schema,
                                       std::string_view _desc) {
    if (_desc.empty()) {
        return _schema;
    }
    return _schema.substr(0, _schema.size() - 1) + ",\"description\":\"" +
           json_escape(_desc) + "\"}";
}

// --- schema generation ------------------------------------------------------

template <typename T> consteval std::string param_schema();

template <typename T> consteval std::size_t member_count() {
    return meta::nonstatic_data_members_of(^^T,
                                           meta::access_context::unchecked())
        .size();
}

template <typename T>
consteval std::array<meta::info, member_count<T>()> member_infos() {
    const auto members =
        meta::nonstatic_data_members_of(^^T, meta::access_context::unchecked());
    std::array<meta::info, member_count<T>()> out{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        out[i] = members[i];
    }
    return out;
}

template <typename T, std::size_t... Is>
consteval std::string object_schema_impl(std::index_sequence<Is...>) {
    constexpr auto infos = member_infos<T>();
    std::string properties;
    std::string required;
    ((void)([&] {
         using MT = std::remove_cvref_t<typename[:meta::type_of(infos[Is]):]>;
         const std::string name(meta::identifier_of(infos[Is]));
         if (!properties.empty()) {
             properties += ",";
         }
         properties += "\"" + name + "\":" +
                       with_description(param_schema<MT>(),
                                        text_of<^^js::desc, infos[Is]>());
         if constexpr (is_optional<MT>::value ||
                       !meta::has_default_member_initializer(infos[Is])) {
             if (!required.empty()) {
                 required += ",";
             }
             required += "\"" + name + "\"";
         }
     }()),
     ...);
    return "{\"type\":\"object\",\"properties\":{" + properties +
           "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

/// anyOf over a variant's alternatives. Each alternative contributes the schema
/// its own type already produces. `anyOf` and not `oneOf`: the exclusive form
/// is rejected by the tool schemas of both OpenAI and Anthropic, and a Python
/// `A | B` is an anyOf too.
template <typename T, std::size_t... Is>
consteval std::string variant_schema_impl(std::index_sequence<Is...>);

/// JSON Schema for one C++ type. std::optional becomes anyOf[T, null] (matching
/// Pydantic's Optional), vectors/arrays become arrays, enums become string
/// enums, variants become anyOf over their alternatives, and other classes
/// become nested objects.
template <typename T> consteval std::string param_schema() {
    if constexpr (is_optional<T>::value) {
        return "{\"anyOf\":[" + param_schema<typename T::value_type>() +
               ",{\"type\":\"null\"}]}";
    } else if constexpr (is_sequence<T>::value) {
        std::string out = "{\"type\":\"array\",\"items\":" +
                          param_schema<typename T::value_type>();
        if constexpr (is_sequence<T>::fixed_size) {
            // std::array has a fixed length: pin it so the model knows the
            // size.
            const std::string n = decimal(is_sequence<T>::extent);
            out += ",\"minItems\":" + n + ",\"maxItems\":" + n;
        }
        out += "}";
        return out;
    } else if constexpr (is_variant<T>::value) {
        return variant_schema_impl<T>(
            std::make_index_sequence<std::variant_size_v<T>>());
    } else if constexpr (std::is_enum_v<T>) {
        std::string values;
        for (meta::info e : meta::enumerators_of(^^T)) {
            if (!values.empty()) {
                values += ",";
            }
            values += "\"" + std::string(meta::identifier_of(e)) + "\"";
        }
        return "{\"type\":\"string\",\"enum\":[" + values + "]}";
    } else if constexpr (std::is_class_v<T> && !is_string_like_v<T>) {
        return object_schema_impl<T>(
            std::make_index_sequence<member_count<T>()>());
    } else {
        static_assert(
            is_json_scalar_v<T>,
            "param_schema: unsupported type for JSON Schema generation "
            "(only bool, integers, floats, strings, options, sequences, enums, "
            "variants and reflected classes are supported)");
        return std::string("{\"type\":\"") + json_type_name(^^T) + "\"}";
    }
}

template <typename T, std::size_t... Is>
consteval std::string variant_schema_impl(std::index_sequence<Is...>) {
    std::string out = "{\"anyOf\":[";
    ((void)([&] {
         out += std::string(Is == 0 ? "" : ",");
         out += param_schema<std::variant_alternative_t<Is, T>>();
     }()),
     ...);
    out += "]}";
    return out;
}

// --- function introspection -------------------------------------------------
// Anything touching meta::info happens at consteval; the results handed to the
// runtime string assembly are plain values.

template <meta::info Fn> consteval std::size_t param_count() {
    return meta::parameters_of(Fn).size();
}

template <meta::info Fn, std::size_t I> consteval meta::info param_at() {
    return meta::parameters_of(Fn)[I];
}

template <meta::info Fn, std::size_t I>
using param_type_at =
    std::remove_cvref_t<typename[:meta::type_of(param_at<Fn, I>()):]>;

template <meta::info Fn, std::size_t... Is>
consteval std::string param_object_schema_impl(std::index_sequence<Is...>) {
    std::string properties;
    std::string required;
    ((void)([&] {
         using PT = param_type_at<Fn, Is>;
         const std::string name(meta::identifier_of(param_at<Fn, Is>()));
         if (!properties.empty()) {
             properties += ",";
         }
         properties +=
             "\"" + name + "\":" +
             with_description(param_schema<PT>(), param_description<Fn, Is>());
         // A default argument does not make a parameter omittable: its value
         // cannot be recovered at dispatch time, so the model must supply it.
         if (!required.empty()) {
             required += ",";
         }
         required += "\"" + name + "\"";
     }()),
     ...);
    return "{\"type\":\"object\",\"properties\":{" + properties +
           "},\"required\":[" + required + "],\"additionalProperties\":false}";
}

template <meta::info Fn> consteval std::string param_object_schema() {
    return param_object_schema_impl<Fn>(
        std::make_index_sequence<param_count<Fn>()>());
}

// --- documentation annotation ----------------------------------------------

/// Function-level description from [[= js::doc{ ... } ]].
template <meta::info Fn> consteval std::string description_of() {
    return text_of<^^js::doc, Fn>();
}

// --- public entry points ----------------------------------------------------

enum class Style {
    JsonSchema, ///< the raw JSON Schema of the parameters/fields
    OpenAiTool, ///< {"type":"function","function":{name, description,
                ///< parameters}}
};

/// Builds the OpenAI tool wrapper as plain text. constexpr (not consteval) so
/// the same code serves both the compile-time schema path and a runtime emitter
/// (--emit-tools) that only has the stored string_views.
constexpr std::string build_openai_tool(std::string_view _name,
                                        std::string_view _description,
                                        std::string_view _parameters) {
    std::string out = "{\"type\":\"function\",\"function\":{\"name\":\"";
    out += _name;
    out += "\",\"description\":\"" + json_escape(_description) +
           "\",\"parameters\":";
    out += _parameters;
    out += "}}";
    return out;
}

/// Materializes that wrapper into static storage. Shared by function_schema and
/// tool_schema so both emit the wrapper identically.
consteval std::string_view as_openai_tool(std::string_view _name,
                                          std::string_view _description,
                                          std::string _parameters) {
    return std::string_view(std::define_static_string(
        build_openai_tool(_name, _description, _parameters)));
}

/// Schema for a function's parameters. With Style::OpenAiTool this is a
/// complete tool the caller can hand straight to an API; with Style::JsonSchema
/// it is the bare parameter object. The wrapper is built here, so callers never
/// assemble one. The description comes from an optional [[= js::doc{...}]] on
/// the function.
template <meta::info Fn, Style S = Style::JsonSchema>
consteval std::string_view function_schema() {
    if constexpr (S == Style::OpenAiTool) {
        return as_openai_tool(meta::identifier_of(Fn), description_of<Fn>(),
                              param_object_schema<Fn>());
    } else {
        return std::string_view(
            std::define_static_string(param_object_schema<Fn>()));
    }
}

/// Schema for a struct's fields, mirroring function_schema. The tool name is
/// the type's identifier, and the description comes from an optional
/// [[= js::doc{...}]] on the type.
template <typename T, Style S = Style::JsonSchema>
consteval std::string_view tool_schema() {
    if constexpr (S == Style::OpenAiTool) {
        return as_openai_tool(
            meta::identifier_of(^^T), description_of<^^T>(),
            object_schema_impl<T>(
                std::make_index_sequence<member_count<T>()>()));
    } else {
        return std::string_view(std::define_static_string(object_schema_impl<T>(
            std::make_index_sequence<member_count<T>()>())));
    }
}

// --- reverse direction: JSON value -> C++ value ------------------------------
// The schema is a one-way projection for the LLM, so this does not parse the
// schema back. It re-reflects the same type/function instead: names and types
// are known at compile time, only the values come from the JSON document.
// Deserialization is driven by the reflection, so any reflected struct works.

/// A missing required field, or a value whose JSON type does not match the
/// reflected C++ type. This is a trust boundary: the document comes from a
/// model, so "540" is not an int and a silently defaulted field is a wrong
/// call, not a convenience. During nested decoding each level prefixes the
/// field's own name.
struct bind_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename T> consteval std::size_t enumerator_count() {
    return meta::enumerators_of(^^T).size();
}

template <typename T>
consteval std::array<meta::info, enumerator_count<T>()> enumerator_infos() {
    const auto es = meta::enumerators_of(^^T);
    std::array<meta::info, enumerator_count<T>()> out{};
    for (std::size_t i = 0; i < es.size(); ++i) {
        out[i] = es[i];
    }
    return out;
}

/// {name, value} table for an enum, materialized as ordinary constants so it
/// can be indexed at runtime -- meta::extract() itself needs a constant
/// expression.
template <typename T> struct enumerator_table {
    static constexpr std::size_t size = enumerator_count<T>();
    std::array<std::string_view, size> names;
    std::array<T, size> values;
};

template <typename T, std::size_t... Is>
consteval enumerator_table<T>
make_enumerator_table_impl(std::index_sequence<Is...>) {
    constexpr auto infos = enumerator_infos<T>();
    return enumerator_table<T>{{std::string_view(std::define_static_string(
                                   meta::identifier_of(infos[Is])))...},
                               {meta::extract<T>(infos[Is])...}};
}

template <typename T> consteval enumerator_table<T> make_enumerator_table() {
    return make_enumerator_table_impl<T>(
        std::make_index_sequence<enumerator_count<T>()>());
}

template <typename T> T enum_from_string(const std::string &_name) {
    constexpr auto table = make_enumerator_table<T>();
    for (std::size_t i = 0; i < table.size; ++i) {
        if (table.names[i] == _name) {
            return table.values[i];
        }
    }
    throw bind_error("invalid " + std::string(meta::identifier_of(^^T)) +
                     " value: " + _name);
}

/// Enum -> its reflected enumerator name, the inverse of enum_from_string().
template <typename T> std::string enum_to_string(T _value) {
    constexpr auto table = make_enumerator_table<T>();
    for (std::size_t i = 0; i < table.size; ++i) {
        if (table.values[i] == _value) {
            return std::string(table.names[i]);
        }
    }
    return "?";
}

template <typename T> T from_json(const nlohmann::json &_j);

/// The member reflection for index `I`, as a constant expression. Reflections
/// never live in a runtime variable -- std::meta::info is a consteval-only
/// type, so this is used directly as a template argument.
template <typename T, std::size_t I> consteval meta::info member_at() {
    return member_infos<T>()[I];
}

template <meta::info M>
void assign_member(auto &_out, const nlohmann::json &_j) {
    using MT = std::remove_cvref_t<typename[:meta::type_of(M):]>;
    constexpr std::string_view name =
        std::define_static_string(meta::identifier_of(M));
    const std::string key(name);
    if (!_j.contains(key)) {
        if constexpr (!is_optional<MT>::value &&
                      !meta::has_default_member_initializer(M)) {
            throw bind_error("missing required member: " + key);
        }
        return;
    }
    try {
        _out.[:M:] = from_json<MT>(_j.at(key));
    } catch (const bind_error &e) {
        throw bind_error(key + ": " + e.what());
    }
}

/// Members absent from the document keep the value from default construction,
/// so declared defaults and std::nullopt survive a partial document. A member
/// with neither a default nor std::optional is required and throws when absent.
template <typename T, std::size_t... Is>
T object_from_json(const nlohmann::json &_j, std::index_sequence<Is...>) {
    T out{};
    (assign_member<member_at<T, Is>()>(out, _j), ...);
    return out;
}

// One variant alternative's own from_json is what decides it, so the name has
// to be declared before the helper that calls it.
template <typename T> T from_json(const nlohmann::json &_j);

/// The name an alternative reports in a decode error, so a failure can say
/// which alternatives were tried.
template <typename T> consteval std::string_view variant_alt_name() {
    if constexpr (meta::has_identifier(^^T)) {
        return std::define_static_string(meta::identifier_of(^^T));
    } else {
        return std::string_view("(unnamed alternative)");
    }
}

/// Tries every alternative against the document and accepts the result only
/// when exactly one fits. Each attempt is caught, because an alternative that
/// does not fit is not an error by itself -- the next one may. Neither zero nor
/// two matches can be resolved by guessing: zero is a document no alternative
/// describes, and two is a document that does not say which alternative it is,
/// so picking one would land a plausible-looking wrong value.
template <typename V, std::size_t... Is>
V variant_from_json_impl(const nlohmann::json &_j, std::index_sequence<Is...>) {
    // Optional, not a default-constructed variant: an alternative need not be
    // default-constructible, and only a match ever builds one.
    std::optional<V> decoded;
    std::size_t matched = 0;
    std::string matched_names;
    std::string failures;
    ((void)([&] {
         using Alt = std::variant_alternative_t<Is, V>;
         try {
             Alt value = from_json<Alt>(_j);
             ++matched;
             if (!decoded) {
                 decoded.emplace(std::in_place_type<Alt>, std::move(value));
             }
             if (!matched_names.empty()) {
                 matched_names += ", ";
             }
             matched_names += variant_alt_name<Alt>();
         } catch (const bind_error &e) {
             if (!failures.empty()) {
                 failures += ", ";
             }
             failures += variant_alt_name<Alt>();
             failures += ": ";
             failures += e.what();
         }
     }()),
     ...);
    if (matched == 1) {
        return std::move(*decoded);
    }
    if (matched == 0) {
        throw bind_error("no variant alternative matched: " + failures);
    }
    throw bind_error("variant is ambiguous: " + std::to_string(matched) +
                     " alternatives matched (" + matched_names + ")");
}

/// JSON value -> C++ value, mirroring param_schema()'s type dispatch. Only the
/// exact JSON type is accepted: "540" is an error, not a coercion into int, and
/// 540.0 is not an int either. The document comes from a model, so a mistyped
/// argument must fail loudly rather than land as a plausible-looking value.
template <typename T> T from_json(const nlohmann::json &_j) {
    if constexpr (is_optional<T>::value) {
        if (_j.is_null()) {
            return std::nullopt;
        }
        return from_json<typename T::value_type>(_j);
    } else if constexpr (is_sequence<T>::value) {
        if (!_j.is_array()) {
            throw bind_error("expected array");
        }
        using V = typename T::value_type;
        if constexpr (requires(T t) { t.push_back(V{}); }) {
            T out{};
            out.reserve(_j.size());
            for (const auto &element : _j) {
                out.push_back(from_json<V>(element));
            }
            return out;
        } else {
            static_assert(is_sequence<T>::fixed_size,
                          "from_json: expected a fixed-size std::array");
            T out{};
            // std::array length is part of its type, so a partial document is
            // an error rather than a silently padded result.
            if (_j.size() != out.size()) {
                throw bind_error("array length mismatch: expected " +
                                 std::to_string(out.size()) + ", got " +
                                 std::to_string(_j.size()));
            }
            for (std::size_t i = 0; i < out.size(); ++i) {
                out[i] = from_json<V>(_j.at(i));
            }
            return out;
        }
    } else if constexpr (is_variant<T>::value) {
        return variant_from_json_impl<T>(
            _j, std::make_index_sequence<std::variant_size_v<T>>());
    } else if constexpr (std::is_enum_v<T>) {
        if (!_j.is_string()) {
            throw bind_error("expected string");
        }
        return enum_from_string<T>(_j.get<std::string>());
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!_j.is_boolean()) {
            throw bind_error("expected boolean");
        }
        return _j.get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        if (!_j.is_number_integer()) {
            throw bind_error("expected integer");
        }
        return _j.get<T>();
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!_j.is_number()) {
            throw bind_error("expected number");
        }
        return _j.get<T>();
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!_j.is_string()) {
            throw bind_error("expected string");
        }
        return _j.get<std::string>();
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        if (!_j.is_string()) {
            throw bind_error("expected string");
        }
        // Borrows from the document, which must outlive the result.
        return std::string_view(_j.get_ref<const std::string &>());
    } else if constexpr (std::is_class_v<T>) {
        if (!_j.is_object()) {
            throw bind_error("expected object");
        }
        return object_from_json<T>(
            _j, std::make_index_sequence<member_count<T>()>());
    } else {
        static_assert(!sizeof(T), "js::from_json: unsupported type");
    }
}

/// Builds the argument tuple from the JSON object and actually calls the
/// function. This is the LLM tool-call path: the model's `arguments` string in,
/// a real C++ call out. An std::optional key may be omitted; every other one
/// must be present, and with the exact type it declares.
template <meta::info Fn, std::size_t I>
void assign_argument(auto &_args, const nlohmann::json &_j) {
    using PT = param_type_at<Fn, I>;
    constexpr std::string_view name =
        std::define_static_string(meta::identifier_of(param_at<Fn, I>()));
    const std::string key(name);
    if (_j.contains(key)) {
        try {
            std::get<I>(_args) = from_json<PT>(_j.at(key));
        } catch (const bind_error &e) {
            throw bind_error(key + ": " + e.what());
        }
    } else if constexpr (!is_optional<PT>::value) {
        throw bind_error("missing required argument: " + key);
    }
}

template <meta::info Fn, std::size_t... Is>
auto invoke_with_json_impl(const nlohmann::json &_j,
                           std::index_sequence<Is...>) {
    std::tuple<param_type_at<Fn, Is>...> args{};
    (assign_argument<Fn, Is>(args, _j), ...);
    return std::apply(&[:Fn:], args);
}

template <meta::info Fn> auto invoke_with_json(const nlohmann::json &_j) {
    return invoke_with_json_impl<Fn>(
        _j, std::make_index_sequence<param_count<Fn>()>());
}

/// The member-function counterpart of invoke_with_json: `Fn` is a non-static
/// member function of `Self`, so the call binds the object. parameters_of()
/// excludes the implicit object parameter, so parameter indices still line up
/// with the schema/prompt projections; only the call form differs.
template <meta::info Fn, typename Self, std::size_t... Is>
auto invoke_member_impl(Self &_self, const nlohmann::json &_j,
                        std::index_sequence<Is...>) {
    std::tuple<param_type_at<Fn, Is>...> args{};
    (assign_argument<Fn, Is>(args, _j), ...);
    return (_self.*&[:Fn:])(std::get<Is>(args)...);
}

template <meta::info Fn, typename Self>
auto invoke_member_with_json(Self &_self, const nlohmann::json &_j) {
    return invoke_member_impl<Fn>(
        _self, _j, std::make_index_sequence<param_count<Fn>()>());
}

// --- serialization: C++ value -> JSON ----------------------------------------
// The inverse of from_json(), used to dump a reconstructed struct generically
// so no per-struct print code is needed.

template <typename T> std::string to_json(const T &_value);

/// Shortest round-trip text for a number. std::to_string guarantees only six
/// decimals for floating point, which both loses precision and dresses up
/// values as "0.750000"; std::to_chars emits the shortest exact form.
template <typename T> std::string number_to_json(T _value) {
    char buf[64];
    const auto result = std::to_chars(buf, buf + sizeof buf, _value);
    return std::string(buf, result.ptr);
}

/// Appends `,"name":value`, or nothing at all for an empty std::optional:
/// absence is how parsing reads "no value", so it is how we write one too.
/// `_first` tracks whether the separating comma is still owed.
template <meta::info M, typename T>
void append_member_json(std::string &_out, const T &_value, bool &_first) {
    using MT = std::remove_cvref_t<typename[:meta::type_of(M):]>;
    if constexpr (is_optional<MT>::value) {
        if (!_value.[:M:]) {
            return;
        }
    }
    if (!_first) {
        _out += ",";
    }
    _first = false;
    constexpr std::string_view name =
        std::define_static_string(meta::identifier_of(M));
    _out += "\"";
    _out += name;
    _out += "\":";
    _out += to_json(_value.[:M:]);
}

template <typename T, std::size_t... Is>
std::string object_to_json(const T &_value, std::index_sequence<Is...>) {
    std::string out = "{";
    bool first = true;
    (append_member_json<member_at<T, Is>(), T>(out, _value, first), ...);
    out += "}";
    return out;
}

template <typename T> std::string to_json(const T &_value) {
    if constexpr (std::is_same_v<T, bool>) {
        return _value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return "\"" + enum_to_string(_value) + "\"";
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        return number_to_json(_value);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "\"" + json_escape(_value) + "\"";
    } else if constexpr (is_optional<T>::value) {
        return _value ? to_json(*_value) : "null";
    } else if constexpr (is_sequence<T>::value) {
        std::string out = "[";
        for (std::size_t i = 0; i < _value.size(); ++i) {
            if (i) {
                out += ",";
            }
            out += to_json(_value[i]);
        }
        return out + "]";
    } else if constexpr (is_variant<T>::value) {
        return std::visit([](const auto &_alt) { return to_json(_alt); },
                          _value);
    } else if constexpr (std::is_class_v<T>) {
        return object_to_json(_value,
                              std::make_index_sequence<member_count<T>()>());
    } else {
        return "null";
    }
}

// --- registry: self-registration, filled by the macro below ------------------
// Models are the only self-registered kind left: tools became class-owned (an
// mcp::Server instance holds its own table), so only the structured-output
// schemas still self-register.

struct model_entry {
    std::string_view name;
    std::string_view schema;
    std::string (*dump)(const nlohmann::json &);
};

inline std::vector<model_entry> &model_registry() {
    static std::vector<model_entry> entries;
    return entries;
}

/// Names must be unique: duplicate model names would silently collapse to one
/// key in the emitted object. The registry is populated by static initializers,
/// so this fires before main() -- a duplicate is a hard startup failure, never
/// a silent misroute.
inline void register_model(model_entry _entry) {
    for (const auto &existing : model_registry()) {
        if (existing.name == _entry.name) {
            std::cerr << "js: duplicate model name '" << _entry.name
                      << "' (names must be unique)\n";
            std::abort();
        }
    }
    model_registry().push_back(std::move(_entry));
}

} // namespace js

// Colocated model registration. Use inside the namespace of the entity, after
// its definition:
//   CPP_REFLECT_MODEL(SearchRequest)     // struct -> JSON Schema, parseable
// A function-local static backs the registry, so registration order across
// translation units is safe.

#define CPP_REFLECT_MODEL(Type)                                                \
    namespace {                                                                \
    std::string js_dump_##Type(const nlohmann::json &_j) {                     \
        return ::js::to_json(::js::from_json<Type>(_j));                       \
    }                                                                          \
    [[maybe_unused]] const int js_reg_model_##Type = [] {                      \
        ::js::register_model(::js::model_entry{                                \
            #Type, ::js::tool_schema<Type, ::js::Style::OpenAiTool>(),         \
            &js_dump_##Type});                                                 \
        return 0;                                                              \
    }();                                                                       \
    }
