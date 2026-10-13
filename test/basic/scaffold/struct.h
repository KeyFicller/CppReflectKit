#pragma once

// Scaffolding for test_struct.cpp: the struct being reflected, the member
// queries, and the two encoders built by walking its reflected member list.
// Included by exactly one test translation unit, so it keeps its file-scoped
// anonymous namespace.

#include <cstring>
#include <meta>
#include <string>
#include <string_view>
#include <type_traits>

#include <yaml-cpp/yaml.h>

namespace {

struct MyStruct {
    int m_pub_i;
    double m_pub_d;
    std::string m_pub_s;
};

consteval std::size_t impl_reflection_member_count() {
    return std::size(
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^MyStruct, std::meta::access_context::unchecked())));
}

template <std::size_t N> consteval std::string_view impl_reflection_member() {
    static_assert(N < impl_reflection_member_count(), "N is out of range");
    return std::meta::identifier_of(
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^MyStruct, std::meta::access_context::unchecked()))[N]);
}

consteval bool impl_reflection_has_member(std::string_view _name) {
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^MyStruct, std::meta::access_context::unchecked()))) {
        if (std::meta::identifier_of(member) == _name) {
            return true;
        }
    }
    return false;
}

template <typename T> std::string impl_reflection_dump(const T &_object) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^T, std::meta::access_context::unchecked()))) {
        out << YAML::Key << std::string(std::meta::identifier_of(member))
            << YAML::Value << _object.[:member:];
    }
    out << YAML::EndMap;
    return out.c_str();
}

template <typename T>
void impl_reflection_load(T &_object, const std::string &_text) {
    const YAML::Node node = YAML::Load(_text.c_str());
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^T, std::meta::access_context::unchecked()))) {
        _object.[:member:] = node[std::string(std::meta::identifier_of(member))]
                                 .as<decltype(_object.[:member:])>();
    }
}

template <typename T> std::string impl_reflection_serialize(const T &_object) {
    std::string bytes;
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^T, std::meta::access_context::unchecked()))) {
        using member_type = decltype(_object.[:member:]);
        if constexpr (std::is_same_v<member_type, std::string>) {
            const int length = static_cast<int>(_object.[:member:].size());
            bytes.append(reinterpret_cast<const char *>(&length),
                         sizeof(length));
            bytes.append(_object.[:member:].c_str(), _object.[:member:].size());
        } else {
            static_assert(std::is_trivially_copyable_v<member_type>,
                          "impl_reflection_serialize: raw-byte copy needs a "
                          "trivially copyable member");
            bytes.append(reinterpret_cast<const char *>(&_object.[:member:]),
                         sizeof(member_type));
        }
    }
    return bytes;
}

template <typename T>
void impl_reflection_deserialize(T &_object, const std::string &_bytes) {
    std::size_t offset = 0;
    template for (constexpr std::meta::info member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(
                      ^^T, std::meta::access_context::unchecked()))) {
        using member_type = decltype(_object.[:member:]);
        if constexpr (std::is_same_v<member_type, std::string>) {
            int length = 0;
            std::memcpy(&length, _bytes.data() + offset, sizeof(length));
            offset += sizeof(length);
            _object.[:member:] = std::string(_bytes.data() + offset,
                                             static_cast<std::size_t>(length));
            offset += static_cast<std::size_t>(length);
        } else {
            static_assert(std::is_trivially_copyable_v<member_type>,
                          "impl_reflection_deserialize: raw-byte copy needs a "
                          "trivially copyable member");
            std::memcpy(&_object.[:member:], _bytes.data() + offset,
                                           sizeof(member_type));
            offset += sizeof(member_type);
        }
    }
}

/// The value both encoders start from and must reproduce.
MyStruct sample() {
    return MyStruct{.m_pub_i = 1, .m_pub_d = 2.0, .m_pub_s = "Hello"};
}

} // namespace
