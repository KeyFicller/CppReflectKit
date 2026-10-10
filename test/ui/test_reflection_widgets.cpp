#include "ui.h"
#include "scaffold/reflection_widgets.h"

#include <functional>

// Compile-time contract: reflections read the field annotations, and the value
// comes through intact. This is the only verification the ui target gets, since
// it opens a window and cannot run under ctest.
static_assert(ui::has_annotation<^^ui::slider, ^^Sensor::m_voltage>(),
              "slider annotation not read");
static_assert(ui::annotation_value<^^ui::slider, ^^Sensor::m_voltage>().v_min == -10,
              "slider v_min not read");
static_assert(ui::annotation_value<^^ui::slider, ^^Sensor::m_voltage>().v_max == 10,
              "slider v_max not read");
static_assert(ui::has_annotation<^^ui::drag, ^^Sensor::m_drift>(), "drag annotation not read");
static_assert(ui::has_annotation<^^ui::input, ^^Sensor::m_retries>(), "input annotation not read");
static_assert(ui::has_annotation<^^ui::checkbox, ^^Sensor::m_enabled>(), "checkbox annotation not read");
static_assert(!ui::has_annotation<^^ui::slider, ^^Calibration::m_channel>(),
              "an unannotated field must not report a control annotation");

// label annotations: read from a field, text intact; absent when unlabelled.
static_assert(ui::has_annotation_template<^^ui::label, ^^Sensor::m_voltage>(), "field label not read");
static_assert(ui::annotation_label<^^Sensor::m_voltage>() == "Input voltage", "field label text");
static_assert(!ui::has_annotation_template<^^ui::label, ^^Calibration::m_channel>(), "unlabelled field");
static_assert(ui::annotation_label<^^Calibration::m_channel>() == "", "unlabelled field label is empty");

// field_label is the label path the walk actually calls; it must give the
// annotation text, or the reflected member name for unlabelled fields.
static_assert(ui::field_label<^^Sensor::m_voltage>() == "Input voltage", "field_label reads the label");
static_assert(ui::field_label<^^Calibration::m_channel>() == "m_channel", "field_label falls back to the name");

extern void run_imgui_demo_window(std::function<void()> my_draw_callback);

void ui_reflection()
{
    auto my_draw_callback = [] {
        static Sensor data{};
        ImGui::Begin("UI Reflection");
        ImGui::TextUnformatted("Auto Reflect Member List.");
        ui::draw(data);
        ImGui::End();
    };

    run_imgui_demo_window(my_draw_callback);
}
