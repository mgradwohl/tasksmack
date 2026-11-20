#pragma once

// TaskSmack's desktop identity on Linux (#4451): the reverse-DNS id its desktop files are named for
// (assets/linux/app.tasksmack.TaskSmack.desktop) and their StartupWMClass. SDL sends it as the Wayland
// xdg_toplevel app_id and the X11 WM_CLASS, so a compositor or dock that matches a window to its
// launcher strictly by id finds the icon and groups the window. tests/tools/test_administrator_launcher.py
// checks the desktop files against this value.

namespace Core
{

inline constexpr const char* DESKTOP_APP_ID = "app.tasksmack.TaskSmack";

} // namespace Core
