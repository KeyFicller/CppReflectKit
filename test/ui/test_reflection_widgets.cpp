#include "ui.h"
#include "scaffold/reflection_widgets.h"

extern void run_imgui_demo_window(std::function<void()> my_draw_callback);

void ui_reflection()
{
    auto my_draw_callback = [] {
        static MyDeriveStruct data{};
        ImGui::Begin("UI Reflection");
        ImGui::TextUnformatted("Auto Reflect Member List.");
        impl_reflection_ui(data);
        ImGui::End();
    };

    run_imgui_demo_window(my_draw_callback);
}
