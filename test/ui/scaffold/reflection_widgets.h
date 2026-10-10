#pragma once

// Scaffolding for test_reflection_widgets.cpp: the demo data types and the
// per-field control annotations. The reflection walk and the control matrix live
// in lib/inc/ui/reflection_widgets.h; every type it knows already has a widget, so
// the work here is just naming one per field with an annotation.
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
    char m_serial[32];                     // char array -> the buf/buf_size InputText
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

    // Per-field annotations override the type's default widget. One annotation
    // names the ImGui function to call and carries its parameters by name. A
    // label annotation replaces the reflected member name with text.
    [[= ui::label{ .text = ui::str("Input voltage") }]]
    [[= ui::control{ .fn = ^^ImGui::SliderInt, .v_min = -10.0, .v_max = 10.0 }]] int m_voltage = 0;
    [[= ui::control{ .fn = ^^ImGui::DragInt, .v_speed = 0.5f, .v_min = -100.0, .v_max = 100.0 }]] int m_drift = 0;
    [[= ui::label{ .text = ui::str("Retries") }]]
    [[= ui::control{ .fn = ^^ImGui::InputInt, .step = 1.0, .step_fast = 10.0 }]] int m_retries = 0;
    [[= ui::control{ .fn = ^^ImGui::Checkbox }]] bool m_enabled = false;
};
