#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h> // ImPlotPlot: the legend entries ImPlot kept from the previous frame

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

float legendNameBudget(std::string_view suffix)
{
    const ImPlotStyle& style = ImPlot::GetStyle();
    // ClampLegendRect keeps an outside legend within the frame less PlotPadding on each side; an
    // entry is the legend's inner padding on each side, a one-line-square icon, then its label.
    const float chrome = (2.0F * style.PlotPadding.x) + (2.0F * style.LegendInnerPadding.x) + ImGui::GetTextLineHeight();
    const float suffixWidth = ImGui::CalcTextSize(suffix.data(), suffix.data() + suffix.size()).x;
    return ImGui::GetContentRegionAvail().x - chrome - suffixWidth;
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
