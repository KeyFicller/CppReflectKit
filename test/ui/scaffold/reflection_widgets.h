#pragma once

// Scaffolding for test_reflection_widgets.cpp: the demo data types and one
// override of a built-in widget. The reflection walk and the control matrix live
// in lib/inc/ui/reflection_widgets.h; the library already knows int and double,
// so here we only show how to replace one of its controls with an app-specific
// widget -- a 0..100 slider instead of the default drag field.
//
// The app itself -- the window callback and the GLFW/OpenGL backend -- stays in
// test_reflection_widgets.cpp and scaffold/imgui_backend_glfw_opengl3.cpp.

#include "ui/reflection_widgets.h"

#include <optional>
#include <string>
#include <vector>

// A three-level hierarchy, so the walk has bases to recurse through first:
//   Sensor is-a Calibration is-a Identity
struct Identity {
    std::string m_name;
};

struct Calibration : public Identity {
    int m_channel;
    double m_gain;
};

enum class Unit {
    Celsius,
    Fahrenheit
};

struct Sensor : public Calibration {
    Calibration m_backup;                  // nested   -> a collapsing group
    Unit m_unit = Unit::Celsius;           // enum     -> a combo
    std::optional<int> m_offset;           // optional -> presence + slider
    std::vector<int> m_samples;            // sequence -> sliders with add/remove
    bool m_armed = false;                  // bool     -> a checkbox

    // Per-field annotations override the type's default widget; the parameter
    // names are ImGui::SliderInt / DragInt / InputInt's own. A label annotation
    // replaces the reflected member name with human text.
    [[= ui::label{ .text = ui::str("Input voltage") }]]
    [[= ui::slider{.v_min = -10, .v_max = 10}]] int m_voltage = 0;
    [[= ui::drag{.v_speed = 0.5f, .v_min = -100, .v_max = 100}]] int m_drift = 0;
    [[= ui::label{ .text = ui::str("Retries") }]]
    [[= ui::input{.step = 1, .step_fast = 10}]] int m_retries = 0;
    [[= ui::checkbox{}]] bool m_enabled = false;
};

namespace ui {

template <>
struct Control<int> {
    void draw(int& value)
    {
        ImGui::PushID(static_cast<const void*>(&value));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::SliderInt("##slider", &value, 0, 100);
        ImGui::PopID();
    }
};

template <>
struct Control<double> {
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

} // namespace ui
