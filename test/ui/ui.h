#pragma once

// The ImGui application: reflection walks a struct's members and draws a widget
// per type. Interactive -- opening a window and running a frame loop -- so it is
// built but not part of the unattended ctest run.

void ui_reflection();
