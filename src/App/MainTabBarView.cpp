#include "MainTabBarView.h"

#include "App/ShellMetrics.h"
#include "App/TitleBarGeometry.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <imgui_internal.h> // BeginTabBarEx(): a bar inset from the window's right edge too

#include <cmath>
#include <cstddef>
#include <ranges>
#include <span>

namespace App::MainTabBarView
{

namespace
{
/// ImGui's widest tab, in ems (TabBarCalcMaxTabWidth() in imgui_widgets.cpp, which is not exported).
constexpr float MAX_TAB_WIDTH_EM = 20.0F;
} // namespace

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

    // BeginTabBar() as ImGui writes it, but with the bar ending the same inset short of the window's
    // right edge as it starts from its left. BeginTabBar() runs the bar to the edge, so in a window
    // too narrow for every tab the last one was drawn flush against it, its corner cut off (#1939).
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    bool barOpen = false;
    if (!window->SkipItems)
    {
        ImGuiContext& context = *ImGui::GetCurrentContext();
        const ImGuiID id = window->GetID("##MainTabBar");
        ImGuiTabBar* tabBar = context.TabBars.GetOrAddByKey(id);
        const float inset = ShellMetrics::CONTENT_PADDING_H * styleScale;
        const ImVec2 cursor = window->DC.CursorPos;
        const ImRect bar(cursor.x,
                         cursor.y,
                         std::fmax(cursor.x, window->WorkRect.Max.x - inset),
                         cursor.y + context.FontSize + (context.Style.FramePadding.y * 2.0F));
        tabBar->ID = id;
        tabBar->SeparatorMinX = bar.Min.x - std::trunc(window->WindowPadding.x * 0.5F);
        tabBar->SeparatorMaxX = bar.Max.x + std::trunc(window->WindowPadding.x * 0.5F);
        barOpen = ImGui::BeginTabBarEx(tabBar,
                                       bar,
                                       ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip |
                                           ImGuiTabBarFlags_DrawSelectedOverline | ImGuiTabBarFlags_IsFocused);
    }
    if (barOpen)
    {
        result.drawn = true;
        for (std::size_t index = 0; index < tabs.size(); ++index)
        {
            ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
            if (tabs[index].selectRequested)
            {
                tabFlags |= ImGuiTabItemFlags_SetSelected;
            }
            const bool open = ImGui::BeginTabItem(tabs[index].label, nullptr, tabFlags);
            // The tab is the last item either way; its icon goes where the label starts, centred in
            // its height (#1800).
            if (tabs[index].icon != ImTextureID_Invalid && tabs[index].iconSize > 0.0F && ImGui::IsItemVisible())
            {
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                const float size = tabs[index].iconSize;
                const ImVec2 iconMin(std::floor(min.x + ImGui::GetStyle().FramePadding.x),
                                     std::floor(min.y + ((max.y - min.y - size) * 0.5F)));
                ImGui::GetWindowDrawList()->AddImage(ImTextureRef(tabs[index].icon), iconMin, ImVec2(iconMin.x + size, iconMin.y + size));
            }
            if (open)
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

float measureWidth(std::span<const char* const> labels)
{
    const float styleScale = UI::Theme::get().styleScale();
    const ImGuiStyle& style = ImGui::GetStyle();
    // Each label as BeginTabItem() sizes it: the text before any "##". Measured every frame, so
    // measured as the sum goes rather than into a buffer.
    return computeMainTabBarWidth(labels |
                                      std::views::transform([](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x; }),
                                  {
                                      .framePaddingXPx = ShellMetrics::TAB_PADDING_X * styleScale,
                                      .tabSpacingPx = style.ItemInnerSpacing.x,
                                      .minTabWidthPx = style.TabMinWidthBase,
                                      .maxTabWidthPx = ImGui::GetFontSize() * MAX_TAB_WIDTH_EM,
                                      .insetPx = ShellMetrics::CONTENT_PADDING_H * styleScale,
                                  });
}

} // namespace App::MainTabBarView
