#include "ui.h"
#include "scaffold/reflection_widgets.h"

#include <functional>

// Compile-time contract: reflections read the field annotations, and the value
// comes through intact. This is the only verification the ui target gets, since
// it opens a window and cannot run under ctest.
static_assert(ui::has_annotation<^^ui::control, ^^Sensor::m_voltage>(),
              "slider annotation not read");
static_assert(ui::annotation_value<^^ui::control, ^^Sensor::m_voltage>().v_min == -10.0,
              "slider v_min not read");
static_assert(ui::annotation_value<^^ui::control, ^^Sensor::m_voltage>().v_max == 10.0,
              "slider v_max not read");
static_assert(ui::annotation_value<^^ui::control, ^^Sensor::m_voltage>().fn == ^^ImGui::SliderInt,
              "the annotation must name the function to call");
static_assert(ui::has_annotation<^^ui::control, ^^Sensor::m_drift>(), "drag annotation not read");
static_assert(ui::has_annotation<^^ui::control, ^^Sensor::m_retries>(), "input annotation not read");
static_assert(ui::has_annotation<^^ui::control, ^^Sensor::m_enabled>(), "checkbox annotation not read");
static_assert(!ui::has_annotation<^^ui::control, ^^Calibration::m_channel>(),
              "an unannotated field must not report a control annotation");

// A hidden field is read as such and carries no control, so the walk skips it
// while a normal field is still drawn.
static_assert(ui::has_annotation<^^ui::hidden, ^^Sensor::m_internal>(),
              "hidden annotation not read");
static_assert(ui::is_hidden<^^Sensor::m_internal>(), "hidden field must report hidden");
static_assert(!ui::is_hidden<^^Sensor::m_armed>(), "an unannotated field is not hidden");
static_assert(!ui::has_annotation<^^ui::control, ^^Sensor::m_internal>(),
              "a hidden field carries no control annotation");

// The control's vocabulary is checked against the functions it can drive: every
// member names a parameter of one of them, and a use site must name one.
static_assert(ui::control_members_are_parameters(), "every ui::control member names a parameter");
static_assert(ui::is_control_function<^^ImGui::SliderInt>(), "SliderInt is drivable");
static_assert(!ui::is_control_function<^^ImGui::TextUnformatted>(), "TextUnformatted is not a control");

// An unannotated value is drawn through its type's default control, so the types
// the walk meets without naming a widget (a bool, a scalar, a string, and the
// values inside optionals and sequences) must all resolve to one.
static_assert(ui::has_default_control<bool> && ui::has_default_control<int> &&
                  ui::has_default_control<double> && ui::has_default_control<std::string> &&
                  ui::has_default_control<char[32]>,
              "the scalar, string and char-array types need a default control");
static_assert(!ui::has_default_control<Sensor> && !ui::has_default_control<std::vector<int>>,
              "a group type must not claim a leaf control");
static_assert(ui::control_traits<ui::type_control<bool>>::function() == ^^ImGui::Checkbox,
              "bool default is a checkbox");
static_assert(ui::control_traits<ui::type_control<int>>::function() == ^^ImGui::DragScalar,
              "scalar default is a drag");
// std::string needs the pinned overload: ^^ImGui::InputText is the char* one, and
// reflect_function on the cast signature reaches the std::string* one.
static_assert(ui::control_traits<ui::type_control<std::string>>::function() == ui::string_input_text(),
              "string default goes through the pinned InputText overload");
static_assert(ui::is_control_function<ui::string_input_text()>(), "the pinned InputText overload is drivable");
static_assert(std::meta::identifier_of(std::meta::parameters_of(ui::string_input_text())[1]) == "str",
              "the pinned overload keeps its parameter name");

// A char array is the char* InputText: it is pinned the same way, its value
// parameter is `buf`, and the size comes from the array's extent.
static_assert(ui::control_traits<ui::type_control<char[32]>>::function() == ui::charptr_input_text(),
              "char array default is the pinned buf InputText");
static_assert(ui::is_control_function<ui::charptr_input_text()>(), "the char* InputText overload is drivable");
static_assert(std::meta::identifier_of(std::meta::parameters_of(ui::charptr_input_text())[1]) == "buf",
              "the char* overload's value parameter is buf");
static_assert(std::meta::identifier_of(std::meta::parameters_of(ui::charptr_input_text())[2]) == "buf_size",
              "the char* overload takes the buffer size");
static_assert(ui::field_label<^^Identity::m_serial>() == "m_serial", "char array field is labelled");

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
