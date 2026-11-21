#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <meta>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "detail/undo_session.h"
#include "detail/undo_value_codec.h"

namespace undo {

template <typename Derived> class DbObject;

namespace detail {

template <typename T> struct Member {
    template <typename... Args>
    Member(Args &&..._args) : m_value(std::forward<Args>(_args)...) {}

    const T &get() const { return m_value; }
    T &touch() { return m_value; }

private:
    T m_value;
};

template <typename T>
consteval std::size_t member_index(std::meta::info target) {
    constexpr auto fields =
        std::define_static_array(std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::unchecked()));
    std::size_t index = 0;
    template for (constexpr std::meta::info field : fields) {
        if (field == target)
            return index;
        ++index;
    }
#if defined(__clang__)
    __builtin_unreachable();
#else
    return index;
#endif
}

consteval bool is_member_holder_type(std::meta::info dealt_ty) {
    if (!std::meta::has_template_arguments(dealt_ty))
        return false;
    auto args =
        std::define_static_array(std::meta::template_arguments_of(dealt_ty));
    if (args.size() != 1uz)
        return false;
    const std::meta::info rebuilt = std::meta::substitute(^^Member, args);
    return std::meta::is_same_type(rebuilt, dealt_ty);
}

template <typename Owner, typename T>
T &touch_member(DbObject<Owner> &object, std::uint32_t index,
                Member<T> &member) {
    if (!object.m_open) {
        throw std::runtime_error("undo::DbObject is not open");
    }
    if (std::ranges::find(object.m_touched, index) == object.m_touched.end()) {
        object.m_touched.push_back(index);
        object.m_before_payload.append(reinterpret_cast<const char *>(&index),
                                       sizeof(index));
        encode(object.m_before_payload, member.get());
    }
    return member.touch();
}

}

/**
 * @brief Derive from DbObject<Derived> and declare tracked fields with
 * MEMBER().
 */
template <typename Derived> class DbObject : private detail::DbObjectBase {
public:
    /**
     * @brief Associates a tracked object with a session.
     * @param _session The session that records changes to the object.
     */
    explicit DbObject(Session &_session) : m_session(&_session) {}

    /**
     * @brief Reports whether the object is open for edits.
     */
    bool is_open() const { return m_open; }

    /**
     * @brief Starts capturing writes in the current command.
     * @throws std::runtime_error If the object is already open.
     */
    void open();

    /**
     * @brief Adds the before and after images of touched fields to the command.
     * @throws std::runtime_error If the object is closed or has edits outside
     * an active command.
     */
    void close();

private:
    template <typename Owner, typename T>
    friend T &detail::touch_member(DbObject<Owner> &, std::uint32_t,
                                   detail::Member<T> &);

    void apply_payload(std::string_view _payload) override {
        auto &self = static_cast<Derived &>(*this);
        constexpr auto fields =
            std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^Derived, std::meta::access_context::unchecked()));

        std::size_t offset = 0;
        while (offset < _payload.size()) {
            if (_payload.size() - offset < sizeof(std::uint32_t)) {
                throw std::runtime_error("undo: truncated payload");
            }
            std::uint32_t index{};
            std::memcpy(&index, _payload.data() + offset, sizeof(index));
            offset += sizeof(index);

            bool applied = false;
            template for (constexpr std::meta::info field : fields) {
                constexpr std::meta::info field_type =
                    std::meta::dealias(std::meta::type_of(field));
                if constexpr (detail::is_member_holder_type(field_type)) {
                    constexpr std::uint32_t kIndex = static_cast<std::uint32_t>(
                        detail::member_index<Derived>(field));
                    if (!applied && index == kIndex) {
                        constexpr auto args = std::define_static_array(
                            std::meta::template_arguments_of(field_type));
                        using value_type = typename[:args[0]:];
                        value_type value{};
                        detail::decode(_payload, offset, value);
                        self.[:field:].touch() = std::move(value);
                        applied = true;
                    }
                }
            }
            if (!applied) {
                throw std::runtime_error(
                    "undo: payload references an unknown member");
            }
        }
    }

    void abort() override {
        if (m_open) {
            apply_payload(m_before_payload);
            m_open = false;
        }
        m_before_payload.clear();
        m_touched.clear();
    }

    std::string build_after_payload() {
        auto &self = static_cast<Derived &>(*this);
        constexpr auto fields =
            std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^Derived, std::meta::access_context::unchecked()));

        std::string out;
        for (const std::uint32_t index : m_touched) {
            bool written = false;
            template for (constexpr std::meta::info field : fields) {
                constexpr std::meta::info field_type =
                    std::meta::dealias(std::meta::type_of(field));
                if constexpr (detail::is_member_holder_type(field_type)) {
                    constexpr std::uint32_t kIndex = static_cast<std::uint32_t>(
                        detail::member_index<Derived>(field));
                    if (!written && index == kIndex) {
                        out.append(reinterpret_cast<const char *>(&kIndex),
                                   sizeof(kIndex));
                        detail::encode(out, self.[:field:].get());
                        written = true;
                    }
                }
            }
            if (!written) {
                throw std::runtime_error("undo: untracked member index");
            }
        }
        return out;
    }

    Session *m_session = nullptr;
    bool m_open = false;
    std::string m_before_payload;
    std::vector<std::uint32_t> m_touched;
};

template <typename Derived> void DbObject<Derived>::open() {
    if (m_open) {
        throw std::runtime_error("undo::DbObject is already open");
    }
    m_open = true;
    m_before_payload.clear();
    m_touched.clear();
    m_session->attach_open(this);
}

template <typename Derived> void DbObject<Derived>::close() {
    if (!m_open) {
        throw std::runtime_error("undo::DbObject is not open");
    }
    m_open = false;
    m_session->detach_open(this);
    if (!m_touched.empty()) {
        m_session->record_change(*this, m_before_payload,
                                 build_after_payload());
    }
    m_before_payload.clear();
    m_touched.clear();
}

}

/**
 * @brief Declares a tracked field with touch_name() and get_name() accessors.
 * @param type The field type.
 * @param name The field name.
 * @details An optional third argument initializes the field.
 */
#define MEMBER(type, name, ...)                                                \
public:                                                                        \
    type &touch_##name() {                                                     \
        using owner_type = std::remove_cvref_t<decltype(*this)>;               \
        constexpr auto index = static_cast<std::uint32_t>(                     \
            ::undo::detail::member_index<owner_type>(^^owner_type::name));     \
        return ::undo::detail::touch_member<owner_type>(*this, index, name);   \
    }                                                                          \
    const type &get_##name() const { return name.get(); }                      \
                                                                               \
private:                                                                       \
    ::undo::detail::Member<type> name { __VA_ARGS__ }
