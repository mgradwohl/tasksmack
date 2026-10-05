#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h> // ImPlotPlot: the legend entries ImPlot kept from the previous frame

#include <algorithm>
#include <cstddef>
#include <functional>
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
            drawMarkerGlyph(drawList, it->marker, centre, radius, cutOut);
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

namespace
{
/// The plot's legend entries, as a hash of their labels in order (with their "##" IDs): what an
/// outside legend's layout depends on.
std::size_t legendEntrySignature(ImPlotItemGroup& items)
{
    auto signature = static_cast<std::size_t>(items.GetLegendCount());
    for (int i = 0; i < items.GetLegendCount(); ++i)
    {
        // boost::hash_combine's mix.
        signature ^= std::hash<std::string_view>{}(items.GetLegendLabel(i)) + 0x9e3779b9U + (signature << 6U) + (signature >> 2U);
    }
    return signature;
}
} // namespace

bool suppressLegendIfEntriesChanged(std::size_t setupSignature)
{
    ImPlotPlot* plot = ImPlot::GetCurrentPlot();
    // The entries plotted this frame: SetupFinish cleared the previous frame's before any were plotted.
    if (plot == nullptr || (plot->Flags & ImPlotFlags_NoLegend) != 0 || legendEntrySignature(plot->Items) == setupSignature)
    {
        return false;
    }
    plot->Flags |= ImPlotFlags_NoLegend;
    return true;
}

void restoreLegend(const char* plotLabel)
{
    if (ImPlotPlot* plot = ImPlot::GetPlot(plotLabel); plot != nullptr)
    {
        plot->Flags &= ~ImPlotFlags_NoLegend;
    }
}

LegendSetup setupLegendDefault()
{
    constexpr ImPlotLegendFlags LEGEND_FLAGS = ImPlotLegendFlags_NoHighlightItem | ImPlotLegendFlags_Outside;
    bool oneRow = true;
    bool columnFits = true;
    std::size_t signature = 0;
    ImPlotPlot* plot = ImPlot::GetCurrentPlot();
    if (plot != nullptr)
    {
        signature = legendEntrySignature(plot->Items);
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
        columnFits = oneRow || legendColumnFits(labelWidths.size(),
                                                ImGui::GetTextLineHeight(),
                                                style.LegendInnerPadding.y,
                                                style.LegendSpacing.y,
                                                plot->FrameRect.GetHeight());
    }
    if (oneRow)
    {
        ImPlot::SetupLegend(ImPlotLocation_NorthWest, LEGEND_FLAGS | ImPlotLegendFlags_Horizontal);
    }
    else if (columnFits)
    {
        // A column above the plot: ImPlot takes an outside column's size out of the canvas's height
        // only at North alone (not NorthEast or NorthWest), leaving the plot its full width.
        ImPlot::SetupLegend(ImPlotLocation_North, LEGEND_FLAGS);
    }
    else
    {
        // Neither fits: hidden before SetupFinish sizes the canvas, so no height is reserved for it.
        // The flag outlives the frame (BeginPlot resets a plot's flags only when the caller's flags change),
        // so HistoryChart clears it after EndPlot (restoreLegend()) and the next frame chooses again.
        plot->Flags |= ImPlotFlags_NoLegend;
        return {.signature = signature, .shown = false};
    }
    return {.signature = signature, .shown = true};
}

} // namespace UI::Widgets
