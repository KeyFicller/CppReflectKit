#pragma once

// Scaffolding for test_reflection_widgets.cpp: the demo data types and the reflection ->
// ImGui widget mapping (MyUIControl plus the recursive impl_reflection_ui walk).
// The app itself -- the window callback and the GLFW/OpenGL backend -- stays in
// test_reflection_widgets.cpp and scaffold/imgui_backend_glfw_opengl3.cpp.

#include <meta>
#include <print>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <imgui.h>
#include <imgui_stdlib.h>

struct MyBaseStruct {
    std::string m_mem_tag;
};

struct MyStruct : public MyBaseStruct {
    int m_mem_int;
    double m_mem_real;
};

struct MyDeriveStruct : public MyStruct {
    MyStruct m_mem_other;
};

template <typename T>
struct MyUIControl {};

template <>
struct MyUIControl<int> {
    void draw(int& value)
    {
        ImGui::PushID(static_cast<const void*>(&value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::SliderInt("##slider", &value, 0, 100);
        ImGui::PopID();
    }
};

template <>
struct MyUIControl<double> {
    void draw(double& value)
    {
        const double vmin = 0.0;
        const double vmax = 100.0;
        ImGui::PushID(static_cast<const void*>(&value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::SliderScalar("##slider", ImGuiDataType_Double, &value, &vmin, &vmax);
        ImGui::PopID();
    }
};

template <>
struct MyUIControl<std::string> {
    void draw(std::string& value)
    {
        ImGui::PushID(static_cast<const void*>(&value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::InputText("##field", &value);
        ImGui::PopID();
    }
};

template <typename T>
void impl_reflection_ui(T&);

template <typename T>
void impl_reflection_class_members(T& _object)
{
    if constexpr (std::meta::is_class_type(^^T)) {

        constexpr auto bases =
            std::define_static_array(std::meta::bases_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info base : bases) {
            constexpr std::meta::info bt = std::meta::dealias(std::meta::type_of(base));
            if constexpr (std::meta::is_class_type(bt)) {
                using base_type = typename[:bt:];
                impl_reflection_ui(static_cast<base_type&>(_object));
            }
        }

        constexpr auto fields =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unchecked()));
        template for (constexpr std::meta::info field : fields) {
            constexpr std::meta::info fty = std::meta::dealias(std::meta::type_of(field));
            const auto label_sv = std::meta::display_string_of(field);
            const std::string label(label_sv.data(), label_sv.size());
            const std::string member_label = std::string("* ") + label;

            if constexpr (std::meta::is_class_type(fty) && !std::meta::is_same_type(fty, ^^std::string)) {
                ImGui::PushID(static_cast<const void*>(std::addressof(_object.[:field:])));
                if (ImGui::CollapsingHeader((member_label + ':').c_str(),
                                            ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
                    ImGui::Indent();
                    impl_reflection_ui(_object.[:field:]);
                    ImGui::Unindent();
                }
                ImGui::PopID();
            } else {
                ImGui::TextUnformatted(member_label.c_str());
                ImGui::SameLine();

                MyUIControl<typename[:fty:]>{}.draw(_object.[:field:]);
            }
        }
    }
}

template <typename T>
void impl_reflection_ui(T& _object)
{
    if constexpr (std::meta::is_same_type(^^std::remove_cvref_t<T>, ^^std::string)) {
        ImGui::PushID(static_cast<const void*>(std::addressof(_object)));
        ImGui::InputText("##string_value", &_object);
        ImGui::PopID();
        return;
    }

    if constexpr (std::meta::is_class_type(^^T)) {

        ImGui::PushID(static_cast<const void*>(std::addressof(_object)));

        const std::string identifier(std::meta::identifier_of(^^T));

        // A CollapsingHeader like the one on the left of the ImGui demo:
        // framed, full width, arrow plus label.
        if (ImGui::CollapsingHeader((identifier + ':').c_str(),
                                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth)) {
            ImGui::Indent();
            impl_reflection_class_members(_object);
            ImGui::Unindent();
        }

        ImGui::PopID();
    } else {
        const std::string text = std::format("{}", _object);
        ImGui::TextUnformatted(text.c_str());
    }
}
