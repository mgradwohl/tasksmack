#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h> // ImPlotPlot: the legend entries ImPlot kept from the previous frame

#include <algorithm>
#include <span>
#include <string_view>
#include <vector>

namespace UI::Widgets
{

bool legendEntriesKnown(const char* plotLabel)
{
    // Looked up before BeginPlot: the entries are still the previous frame's (SetupFinish resets them).
    const ImPlotPlot* plot = ImPlot::GetPlot(plotLabel);
    return plot != nullptr && plot->Items.GetLegendCount() > 0;
}

namespace
{
/// One marker shape of @p radius at @p centre, filled (or stroked, for the line-only Cross and Plus).
void drawMarkerShape(ImDrawList& drawList, ImPlotMarker marker, ImVec2 centre, float radius, ImU32 colour)
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
} // namespace

void drawLegendMarkers(const char* plotLabel, std::span<const Detail::LegendMarker> markers)
{
    ImPlotPlot* plot = ImPlot::GetPlot(plotLabel);
    if (plot == nullptr || (plot->Flags & ImPlotFlags_NoLegend) != 0 || plot->Items.GetLegendCount() == 0)
    {
        return;
    }
    ImPlotItemGroup& items = plot->Items; // GetLegendLabel() is not const
    const ImPlotLegend& legend = items.Legend;
    const bool vertical = (legend.Flags & ImPlotLegendFlags_Horizontal) == 0;
    const ImPlotStyle& style = ImPlot::GetStyle();
    const float lineHeight = ImGui::GetTextLineHeight();
    // ShowLegendEntries shrinks the key square by 2 px on each side; the shape sits inside that.
    const float radius = std::max(1.0F, ((lineHeight * 0.5F) - 2.0F) * 0.6F);
    const ImU32 cutOut = ImPlot::GetStyleColorU32(ImPlotCol_LegendBg);

    ImDrawList& drawList = *ImGui::GetWindowDrawList();
    drawList.PushClipRect(legend.RectClamped.Min, legend.RectClamped.Max, true);
    float labelWidthsBefore = 0.0F;
    for (int i = 0; i < items.GetLegendCount(); ++i)
    {
        const char* label = items.GetLegendLabel(i);
        const auto it = std::ranges::find_if(markers, [label](const Detail::LegendMarker& m) { return m.label == label; });
        if (it != markers.end())
        {
            // legend.Rect is the scrolled rect EndPlot drew the entries in.
            const ImVec2 centre =
                legendKeyCentre(legend.Rect.Min, style.LegendInnerPadding, style.LegendSpacing, lineHeight, i, labelWidthsBefore, vertical);
            drawMarkerShape(drawList, it->marker, centre, radius, cutOut);
        }
        labelWidthsBefore += ImGui::CalcTextSize(label, nullptr, true).x;
    }
    drawList.PopClipRect();
}

float legendNameBudget(std::string_view suffix, float reservedWidth)
{
    const ImPlotStyle& style = ImPlot::GetStyle();
    // ClampLegendRect keeps an outside legend within the frame less PlotPadding on each side; an
    // entry is the legend's inner padding on each side, a one-line-square icon, then its label.
    const float chrome = (2.0F * style.PlotPadding.x) + (2.0F * style.LegendInnerPadding.x) + ImGui::GetTextLineHeight();
    const float suffixWidth = ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x;
    return ImGui::GetContentRegionAvail().x - reservedWidth - chrome - suffixWidth;
}

void setupLegendDefault()
{
    constexpr ImPlotLegendFlags LEGEND_FLAGS = ImPlotLegendFlags_NoHighlightItem | ImPlotLegendFlags_Outside;
    bool oneRow = true;
    if (ImPlotPlot* plot = ImPlot::GetCurrentPlot(); plot != nullptr)
    {
        // Until the plot's first frame ends there are no entries, and an empty legend fits.
        ImPlotItemGroup& items = plot->Items;
        static std::vector<float> labelWidths; // UI thread only; reused
        labelWidths.clear();
        for (int i = 0; i < items.GetLegendCount(); ++i)
        {
            labelWidths.push_back(ImGui::CalcTextSize(items.GetLegendLabel(i), nullptr, true).x);
        }
        const ImPlotStyle& style = ImPlot::GetStyle();
        // An outside legend is kept within the frame less PlotPadding on each side (ClampLegendRect),
        // and its icons are one text line square (CalcLegendSize).
        const float available = plot->FrameRect.GetWidth() - (2.0F * style.PlotPadding.x);
        oneRow = legendFitsOneRow(labelWidths, ImGui::GetTextLineHeight(), style.LegendInnerPadding.x, style.LegendSpacing.x, available);
    }
    if (oneRow)
    {
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, LEGEND_FLAGS | ImPlotLegendFlags_Horizontal);
    }
    else
    {
        // A column above the plot: ImPlot takes an outside column's size out of the canvas's height
        // only at North alone (not NorthEast or NorthWest), leaving the plot its full width.
        ImPlot::SetupLegend(ImPlotLocation_North, LEGEND_FLAGS);
    }
}

} // namespace UI::Widgets
