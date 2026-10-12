#pragma once

// The main tab bar across the top of the window (#1547): one ImGui tab per panel. ShellLayer decides
// which tab is asked to be selected (a ShowProcessDetailsEvent, TASKSMACK_TAB at startup) and keeps the
// selection and its events; this draws the bar and reports which tab ImGui shows selected, so the bar
// can be rendered headless in the tests.

#include <imgui.h>

#include <cstddef>
#include <optional>
#include <span>

namespace App::MainTabBarView
{

/// One tab this frame.
struct Tab
{
    const char* label = "";       ///< Its text, with a "###" ID of its own (TabLabel.h)
    bool selectRequested = false; ///< Select it this frame (ImGuiTabItemFlags_SetSelected)
    /// An image drawn at the start of the label, over room the label leaves for it with leading
    /// spaces (the Process Details tab's program icon, #1800); ImTextureID_Invalid: none.
    ImTextureID icon = ImTextureID_Invalid;
    float iconSize = 0.0F; ///< The icon's side, in pixels
};

/// What the bar did this frame.
struct Result
{
    bool drawn = false;                  ///< The tab bar was submitted at all
    std::optional<std::size_t> selected; ///< The tab ImGui shows selected, by index into the tabs given
};

/// Draw the main tab bar for this frame.
[[nodiscard]] Result render(std::span<const Tab> tabs);

} // namespace App::MainTabBarView
