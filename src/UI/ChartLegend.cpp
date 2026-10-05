#include "UI/ChartWidgets.h"

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h> // ImPlotPlot: the legend entries ImPlot kept from the previous frame

#include <vector>

namespace UI::Widgets
{

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
        ImPlot::SetupLegend(ImPlotLocation_NorthEast, LEGEND_FLAGS);
    }
}

} // namespace UI::Widgets
