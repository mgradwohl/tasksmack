#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace UI::Widgets
{

void drawMarkerGlyph(ImDrawList& drawList, ImPlotMarker marker, ImVec2 centre, float radius, ImU32 colour)
{
    const float r = radius;
    const float thickness = std::max(1.0F, r * 0.45F);
    switch (marker)
    {
    case ImPlotMarker_Square:
        drawList.AddRectFilled({centre.x - r, centre.y - r}, {centre.x + r, centre.y + r}, colour);
        break;
    case ImPlotMarker_Diamond:
        drawList.AddQuadFilled(
            {centre.x, centre.y - r}, {centre.x + r, centre.y}, {centre.x, centre.y + r}, {centre.x - r, centre.y}, colour);
        break;
    case ImPlotMarker_Up:
        drawList.AddTriangleFilled({centre.x, centre.y - r}, {centre.x + r, centre.y + r}, {centre.x - r, centre.y + r}, colour);
        break;
    case ImPlotMarker_Cross:
        drawList.AddLine({centre.x - r, centre.y - r}, {centre.x + r, centre.y + r}, colour, thickness);
        drawList.AddLine({centre.x - r, centre.y + r}, {centre.x + r, centre.y - r}, colour, thickness);
        break;
    case ImPlotMarker_Plus:
        drawList.AddLine({centre.x - r, centre.y}, {centre.x + r, centre.y}, colour, thickness);
        drawList.AddLine({centre.x, centre.y - r}, {centre.x, centre.y + r}, colour, thickness);
        break;
    default:
        drawList.AddCircleFilled(centre, r, colour);
        break;
    }
}

float seriesNameBudget(std::string_view suffix, float reservedWidth)
{
    // An entry is its swatch and inner spacing, "<name><suffix>:", inner spacing, then the value; a
    // rate value is at most about "999.9 MB/s" wide.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float swatch = std::floor(ImGui::GetTextLineHeight() * TOOLTIP_SWATCH_LINE_FRACTION) + style.ItemInnerSpacing.x;
    const float suffixWidth = ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x + ImGui::CalcTextSize(":").x;
    const float valueWidth = style.ItemInnerSpacing.x + ImGui::CalcTextSize("999.9 MB/s").x;
    return ImGui::GetContentRegionAvail().x - reservedWidth - swatch - suffixWidth - valueWidth;
}

} // namespace UI::Widgets
