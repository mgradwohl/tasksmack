#include "MainTabBarView.h"

#include "App/ShellMetrics.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <cstddef>
#include <span>

namespace App::MainTabBarView
{

Result render(std::span<const Tab> tabs)
{
    Result result;
    // Every size here is authored at the reference configuration and scaled like the ImGuiStyle
    // values around it. As pixel literals pushed over a scaled style they kept one size while the
    // text they frame grew and shrank: the tabs' padding was more than a line of text at Small and
    // about half a line at Even Huger (#971).
    const float styleScale = UI::Theme::get().styleScale();

    // Add top edge padding for visual balance
    ImGui::Dummy(ImVec2(0.0F, ShellMetrics::TOP_EDGE_PADDING * styleScale));

    // Add left indent to align main tabs with panel tabs below (same as the content area's gutter)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ShellMetrics::CONTENT_PADDING_H * styleScale));

    // Add horizontal padding inside tabs and vertical padding for taller tabs
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * styleScale, ShellMetrics::MAIN_TAB_PADDING_Y * styleScale));

    if (ImGui::BeginTabBar("##MainTabBar",
                           ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip |
                               ImGuiTabBarFlags_DrawSelectedOverline))
    {
        result.drawn = true;
        for (std::size_t index = 0; index < tabs.size(); ++index)
        {
            ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
            if (tabs[index].selectRequested)
            {
                tabFlags |= ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tabs[index].label, nullptr, tabFlags))
            {
                result.selected = index;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::PopStyleVar();
    return result;
}

} // namespace App::MainTabBarView
