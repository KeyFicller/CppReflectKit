#pragma once

#include "json_schema.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
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

namespace js {

/**
 * @brief Reports a missing key, unexpected key, or invalid value during
 * binding.
 */
struct bind_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Decodes a reflected value from JSON.
 * @tparam T The C++ type to decode.
 * @param _j The JSON input. It must outlive any decoded string views.
 * @throws bind_error If the JSON does not match the reflected type.
 */
template <typename T> T from_json(const nlohmann::json &_j);

/**
 * @brief Encodes a reflected value as JSON text.
 * @details Empty optional members are omitted.
 * @param _value The C++ value to encode.
 */
template <typename T> std::string to_json(const T &_value);

namespace detail {

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

template <typename T> struct enumerator_table {
    static constexpr std::size_t size = enumerator_count<T>();
    std::array<std::string_view, size> names;
    std::array<T, size> values;
};

template <typename T, std::size_t... Is>
consteval enumerator_table<T>
make_enumerator_table_impl(std::index_sequence<Is...>) {
    constexpr auto infos = enumerator_infos<T>();
    return enumerator_table<T>{{static_text(meta::identifier_of(infos[Is]))...},
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

template <typename T, std::size_t I> consteval meta::info member_at() {
    return member_infos<T>()[I];
}

template <typename T, std::size_t... Is>
consteval std::array<std::string_view, sizeof...(Is)>
member_names(std::index_sequence<Is...>) {
    constexpr auto infos = member_infos<T>();
    return {static_text(meta::identifier_of(infos[Is]))...};
}

template <meta::info Fn, std::size_t... Is>
consteval std::array<std::string_view, sizeof...(Is)>
param_names(std::index_sequence<Is...>) {
    return {static_text(meta::identifier_of(param_at<Fn, Is>()))...};
}

template <std::size_t N>
void reject_unknown_keys(const nlohmann::json &_j,
                         const std::array<std::string_view, N> &_names) {
    for (const auto &item : _j.items()) {
        const std::string &key = item.key();
        if (!std::ranges::contains(_names, key)) {
            throw bind_error("unknown key: " + key);
        }
    }
}

template <meta::info M>
void assign_member(auto &_out, const nlohmann::json &_j) {
    using MT = std::remove_cvref_t<typename[:meta::type_of(M):]>;
    constexpr std::string_view name = static_text(meta::identifier_of(M));
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

template <typename T, std::size_t... Is>
T object_from_json(const nlohmann::json &_j, std::index_sequence<Is...>) {
    constexpr auto names = member_names<T>(std::index_sequence<Is...>{});
    reject_unknown_keys(_j, names);
    T out{};
    (assign_member<member_at<T, Is>()>(out, _j), ...);
    return out;
}

template <typename T> consteval std::string_view variant_alt_name() {
    if constexpr (meta::has_identifier(^^T)) {
        return static_text(meta::identifier_of(^^T));
    } else {
        return std::string_view("(unnamed alternative)");
    }
}

template <typename V, std::size_t... Is>
V variant_from_json_impl(const nlohmann::json &_j, std::index_sequence<Is...>) {
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

template <meta::info Fn, std::size_t I>
void assign_argument(auto &_args, const nlohmann::json &_j) {
    using PT = param_type_at<Fn, I>;
    constexpr std::string_view name =
        static_text(meta::identifier_of(param_at<Fn, I>()));
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
auto bind_arguments(const nlohmann::json &_j, std::index_sequence<Is...>) {
    if (!_j.is_object()) {
        throw bind_error("expected arguments object");
    }
    constexpr auto names = param_names<Fn>(std::index_sequence<Is...>{});
    reject_unknown_keys(_j, names);
    std::tuple<param_type_at<Fn, Is>...> args{};
    (assign_argument<Fn, Is>(args, _j), ...);
    return args;
}

template <meta::info Fn, std::size_t... Is>
auto invoke_with_json_impl(const nlohmann::json &_j,
                           std::index_sequence<Is...> _indices) {
    auto args = bind_arguments<Fn>(_j, _indices);
    return std::apply(&[:Fn:], args);
}

template <meta::info Fn, typename Self, std::size_t... Is>
auto invoke_member_impl(Self &_self, const nlohmann::json &_j,
                        std::index_sequence<Is...> _indices) {
    auto args = bind_arguments<Fn>(_j, _indices);
    return (_self.*&[:Fn:])(std::get<Is>(args)...);
}

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
    constexpr std::string_view name = static_text(meta::identifier_of(M));
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

struct model_entry {
    std::string_view name;
    std::string_view schema;
    std::string (*dump)(const nlohmann::json &);
};

inline std::vector<model_entry> &registered_models() {
    static std::vector<model_entry> entries;
    return entries;
}

template <typename T> bool register_model(std::string_view _name) {
    for (const auto &existing : registered_models()) {
        if (existing.name == _name) {
            std::cerr << "js: duplicate model name '" << _name
                      << "' (names must be unique)\n";
            std::abort();
        }
    }
    registered_models().push_back({_name, tool_schema<T, Style::OpenAiTool>(),
                                   [](const nlohmann::json &_j) {
                                       return ::js::to_json(
                                           ::js::from_json<T>(_j));
                                   }});
    return true;
}

}

/**
 * @brief Looks up a reflected enumerator name.
 * @param _value The enumeration value to look up.
 * @return The enumerator name or "?" if the value has no name.
 */
template <typename T> std::string enum_to_string(T _value) {
    constexpr auto table = detail::make_enumerator_table<T>();
    for (std::size_t i = 0; i < table.size; ++i) {
        if (table.values[i] == _value) {
            return std::string(table.names[i]);
        }
    }
    return "?";
}

template <typename T> T from_json(const nlohmann::json &_j) {
    if constexpr (detail::is_optional<T>::value) {
        if (_j.is_null()) {
            return std::nullopt;
        }
        return from_json<typename T::value_type>(_j);
    } else if constexpr (detail::is_sequence<T>::value) {
        if (!_j.is_array()) {
            throw bind_error("expected array");
        }
        using V = typename T::value_type;
        if constexpr (!detail::is_sequence<T>::fixed_size) {
            T out{};
            out.reserve(_j.size());
            for (const auto &element : _j) {
                out.push_back(from_json<V>(element));
            }
            return out;
        } else {
            T out{};
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
    } else if constexpr (detail::is_variant<T>::value) {
        return detail::variant_from_json_impl<T>(
            _j, std::make_index_sequence<std::variant_size_v<T>>());
    } else if constexpr (std::is_enum_v<T>) {
        if (!_j.is_string()) {
            throw bind_error("expected string");
        }
        return detail::enum_from_string<T>(_j.get<std::string>());
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
    } else if constexpr (detail::is_string_like_v<T>) {
        if (!_j.is_string()) {
            throw bind_error("expected string");
        }
        return T(_j.get_ref<const std::string &>());
    } else if constexpr (std::is_class_v<T>) {
        if (!_j.is_object()) {
            throw bind_error("expected object");
        }
        return detail::object_from_json<T>(
            _j, std::make_index_sequence<detail::member_count<T>()>());
    } else {
        static_assert(!sizeof(T), "js::from_json: unsupported type");
    }
}

/**
 * @brief Invokes a function by binding its parameters from JSON.
 * @tparam Fn Reflection of the function to invoke.
 * @param _j JSON object with arguments keyed by parameter name.
 * @throws bind_error If argument binding fails.
 */
template <std::meta::info Fn> auto invoke_with_json(const nlohmann::json &_j) {
    return detail::invoke_with_json_impl<Fn>(
        _j, std::make_index_sequence<detail::param_count<Fn>()>());
}

/**
 * @brief Invokes a non-static member function with JSON arguments.
 * @tparam Fn Reflection of the member function to invoke.
 * @param _self The object to invoke the member on.
 * @param _j JSON object with arguments keyed by parameter name.
 * @throws bind_error If argument binding fails.
 */
template <std::meta::info Fn, typename Self>
auto invoke_member_with_json(Self &_self, const nlohmann::json &_j) {
    return detail::invoke_member_impl<Fn>(
        _self, _j, std::make_index_sequence<detail::param_count<Fn>()>());
}

template <typename T> std::string to_json(const T &_value) {
    if constexpr (std::is_same_v<T, bool>) {
        return _value ? "true" : "false";
    } else if constexpr (std::is_enum_v<T>) {
        return "\"" + enum_to_string(_value) + "\"";
    } else if constexpr (std::is_integral_v<T> || std::is_floating_point_v<T>) {
        return detail::number_to_json(_value);
    } else if constexpr (detail::is_string_like_v<T>) {
        return "\"" + detail::json_escape(_value) + "\"";
    } else if constexpr (detail::is_optional<T>::value) {
        return _value ? to_json(*_value) : "null";
    } else if constexpr (detail::is_sequence<T>::value) {
        std::string out = "[";
        for (std::size_t i = 0; i < _value.size(); ++i) {
            if (i) {
                out += ",";
            }
            out += to_json(_value[i]);
        }
        return out + "]";
    } else if constexpr (detail::is_variant<T>::value) {
        return std::visit([](const auto &_alt) { return to_json(_alt); },
                          _value);
    } else if constexpr (std::is_class_v<T>) {
        return detail::object_to_json(
            _value, std::make_index_sequence<detail::member_count<T>()>());
    } else {
        return "null";
    }
}

/**
 * @brief Returns the models registered with CPP_REFLECT_MODEL before main().
 */
inline const auto &model_registry() { return detail::registered_models(); }

}

/**
 * @brief Registers a model declared in the current namespace.
 */
#define CPP_REFLECT_MODEL(Type)                                                \
    [[maybe_unused]] static const bool js_reg_model_##Type =                   \
        ::js::detail::register_model<Type>(#Type);
