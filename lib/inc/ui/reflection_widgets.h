#pragma once

#include "../detail/type_traits.h"
#include "detail/widget_annotations.h"
#include "detail/widget_controls.h"

#include <array>
#include <cstddef>
#include <memory>
#include <meta>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <imgui.h>

namespace ui {

/**
 * @brief Draws an editable widget or form for a reflected value.
 * @param[in,out] _object The value to display and edit.
 */
template <typename T> void draw(T &_object);

namespace detail {

using cppreflect::detail::is_optional;
using cppreflect::detail::is_sequence;

template <typename T>
inline constexpr bool is_nested_struct_v =
    std::is_class_v<T> && !std::is_same_v<std::remove_cv_t<T>, std::string> &&
    !is_optional<T>::value && !is_sequence<T>::value && !has_control<T>;

template <typename E> consteval std::string_view enum_names_packed() {
    std::string out;
    for (std::meta::info e : std::meta::enumerators_of(^^E)) {
        out += std::string(std::meta::identifier_of(e));
        out.push_back('\0');
    }
    out.push_back('\0');
    return cppreflect::detail::static_text(out);
}

template <typename E> consteval auto enum_values() {
    constexpr auto infos =
        std::define_static_array(std::meta::enumerators_of(^^E));
    std::array<std::underlying_type_t<E>, infos.size()> out{};
    std::size_t i = 0;
    template for (constexpr std::meta::info e : infos) {
        out[i++] = static_cast<std::underlying_type_t<E>>([:e:]);
    }
    return out;
}

template <typename E> void draw_enum(E &_value) {
    constexpr auto packed = enum_names_packed<E>();
    constexpr auto values = enum_values<E>();
    using underlying = std::underlying_type_t<E>;

    int current = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (static_cast<underlying>(_value) == values[i]) {
            current = static_cast<int>(i);
            break;
        }
    }

    ImGui::PushID(std::addressof(_value));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::Combo("##enum", &current, packed.data(),
                     static_cast<int>(values.size()))) {
        _value = static_cast<E>(values[static_cast<std::size_t>(current)]);
    }
    ImGui::PopID();
}

template <typename T> void draw_optional(std::optional<T> &_value) {
    ImGui::PushID(std::addressof(_value));
    bool present = _value.has_value();
    if (ImGui::Checkbox("##present", &present)) {
        if (present)
            _value.emplace();
        else
            _value.reset();
    }
    ImGui::SameLine();
    if (_value) {
        draw(*_value);
    } else {
        ImGui::TextDisabled("(empty)");
    }
    ImGui::PopID();
}

template <typename V> void draw_sequence(V &_value) {
    ImGui::PushID(std::addressof(_value));

    bool erase = false;
    std::size_t erase_at = 0;
    for (std::size_t i = 0; i < _value.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if constexpr (!is_sequence<V>::fixed_size) {
            if (ImGui::SmallButton("x")) {
                erase = true;
                erase_at = i;
            }
            ImGui::SameLine();
        }
        draw(_value[i]);
        ImGui::PopID();
    }
    if constexpr (!is_sequence<V>::fixed_size) {
        if (erase) {
            _value.erase(_value.begin() +
                         static_cast<std::ptrdiff_t>(erase_at));
        }
        if (ImGui::SmallButton("+ Add")) {
            _value.emplace_back();
        }
    }

    ImGui::PopID();
}

inline bool begin_field_table() {
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable("##fields", 2, flags))
        return false;
    ImGui::TableSetupColumn("field", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

template <typename Fn> void field_row(const std::string &_label, Fn &&_widget) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(_label.c_str());
    ImGui::TableNextColumn();
    _widget();
}

template <typename T> void draw_members(T &_object) {
    if constexpr (std::meta::is_class_type(^^T)) {
        constexpr auto bases = std::define_static_array(
            std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info base : bases) {
            constexpr std::meta::info bt =
                std::meta::dealias(std::meta::type_of(base));
            if constexpr (std::meta::is_class_type(bt)) {
                using base_type = typename[:bt:];
                draw_members(static_cast<base_type &>(_object));
            }
        }

        constexpr auto fields =
            std::define_static_array(std::meta::nonstatic_data_members_of(
                ^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty =
                std::meta::dealias(std::meta::type_of(field));
            if constexpr (is_hidden<field>()) {
                continue;
            }
            const std::string member_label = field_label<field>();

            if constexpr (is_widget_annotated<field>()) {
                field_row(member_label, [&] {
                    draw_control<field_control<field>>("##value",
                                                       _object.[:field:]);
                });
            } else if constexpr (is_nested_struct_v<typename[:fty:]>) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool open =
                    ImGui::TreeNodeEx(std::addressof(_object.[:field:]),
                                      ImGuiTreeNodeFlags_DefaultOpen |
                                          ImGuiTreeNodeFlags_SpanAllColumns,
                                      "%s", member_label.c_str());
                if (open) {
                    draw_members(_object.[:field:]);
                    ImGui::TreePop();
                }
            } else {
                field_row(member_label, [&] { draw(_object.[:field:]); });
            }
        }
    }
}

}

template <typename T> void draw(T &_object) {
    if constexpr (detail::has_control<T>) {
        Control<T>{}.draw(_object);
    } else if constexpr (detail::has_default_control<T>) {
        draw_control<type_control<T>>("##value", _object);
    } else if constexpr (detail::is_optional<T>::value) {
        detail::draw_optional(_object);
    } else if constexpr (detail::is_sequence<T>::value) {
        detail::draw_sequence(_object);
    } else if constexpr (std::is_enum_v<T>) {
        detail::draw_enum(_object);
    } else if constexpr (std::is_class_v<T>) {
        ImGui::PushID(std::addressof(_object));
        const std::string identifier(std::meta::identifier_of(^^T));
        if (ImGui::CollapsingHeader(identifier.c_str(),
                                    ImGuiTreeNodeFlags_DefaultOpen)) {
            if (detail::begin_field_table()) {
                detail::draw_members(_object);
                ImGui::EndTable();
            }
        }
        ImGui::PopID();
    } else {
        static_assert(
            !sizeof(T),
            "ui::draw: no Control<T> for this type, and it is not an enum, "
            "optional, sequence or reflected class");
    }
}

}
