#pragma once

// Draws a Processes row's inline meter (#1528) behind the current table cell's text. See
// ProcessRowMeter.h for the scaling and the settings; this is the ImGui half.

#include "App/Panels/ProcessRowMeter.h"
#include "App/ProcessColumnConfig.h"
#include "Domain/ProcessSnapshot.h"

#include <imgui.h>

namespace App::ProcessRowMeter
{

/// The heat gradient's ends, from the theme ([process_meter] low and high).
struct Colors
{
    ImVec4 low;
    ImVec4 high;
};

/// Bars narrower than this many pixels are not drawn: they would not be seen.
inline constexpr float MIN_BAR_WIDTH_PX = 1.0F;

/// Draws @p col's meter for @p proc behind the current table cell's content, if @p settings has it
/// on; call after ImGui::TableSetColumnIndex() and before the cell's text, so the text lands on top.
/// One AddRectFilled at most: no widget, no ID, no allocation. Returns whether it drew a bar.
inline bool renderCellMeter(
    const Settings& settings, ProcessColumn col, const Domain::ProcessSnapshot& proc, const ColumnMaxima& maxima, const Colors& colors)
{
    if (!settings.isOn(col))
    {
        return false;
    }
    const float fraction = fractionOf(proc, col, maxima);
    const float width = ImGui::GetContentRegionAvail().x * fraction;
    if (!(width >= MIN_BAR_WIDTH_PX))
    {
        return false;
    }
    // The cell's content box, grown by half the cell padding above and below so the bars of one
    // column read as a continuous strip with a hairline between rows.
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float padY = ImGui::GetStyle().CellPadding.y * 0.5F;
    const ImVec4 tint(colors.low.x + ((colors.high.x - colors.low.x) * fraction),
                      colors.low.y + ((colors.high.y - colors.low.y) * fraction),
                      colors.low.z + ((colors.high.z - colors.low.z) * fraction),
                      colors.low.w + ((colors.high.w - colors.low.w) * fraction));
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(pos.x, pos.y - padY),
                                              ImVec2(pos.x + width, pos.y + ImGui::GetTextLineHeight() + padY),
                                              ImGui::ColorConvertFloat4ToU32(tint));
    return true;
}

} // namespace App::ProcessRowMeter
