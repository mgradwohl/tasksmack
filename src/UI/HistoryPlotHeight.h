#pragma once

// The one rule for how tall a history chart is, shared by the tabs that stack several of them.
//
// Two sibling tabs used to implement opposite policies. The system Overview gave every chart a fixed
// 180px, so on a tall window up to a third of it sat empty beneath them (#922). Network and I/O let
// its per-disk charts take all remaining height with no upper bound, so on the same window each one
// was ~920px of flat line (#923). The rule both now follow:
//
//     a chart grows to share the height available, between a minimum and a maximum.
//
// The bounds are in ems (ImGui::GetFontSize(), which carries both the Font Size setting and the
// display's density) so a chart keeps its proportions against its own axis labels:
//
//   - below the minimum the Y-axis tick labels crowd each other, so the tab scrolls instead;
//   - above the maximum the extra height shows nothing more -- the data has no more detail to give --
//     and the space is better left to the other sections on the tab.
//
// The pure arithmetic lives here so it is unit-testable without a live ImGui context, following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace UI::Widgets
{

/// Shortest a stacked history chart may be, in body ems: 90px at the Medium preset on a 1.0 display
/// scale (em = 32/3 px) and 180px at Even Huger. The chart-text floor below is the larger of the two
/// at Small and Medium, where chart text is the body size.
///
/// Chosen so that Even Huger, where the axis labels are largest, keeps exactly the 180px these
/// charts used to be fixed at. A larger multiple would make the charts taller than before at the
/// big presets and push a tab that used to fit into scrolling.
inline constexpr float HISTORY_PLOT_MIN_HEIGHT_EM = 8.4375F;

/// Shortest a stacked history chart may be in ems of its own axis and legend text, which can be the
/// body size (see UI::chartFontSize()): 11.25 is the proportion the Medium preset had when its chart
/// text was 8px in a 90px chart, which keeps a four-entry legend (Memory: Used, Cached, Swap, Peak
/// Used) and six Y-axis labels inside the plot. Moving chart text up to the body size at Medium
/// (#1194) without this would have clipped that legend.
inline constexpr float HISTORY_PLOT_MIN_HEIGHT_CHART_EM = 11.25F;

/// Tallest a stacked history chart may grow, in ems: 360px at the Medium preset, twice the 180px
/// these charts used to be fixed at.
inline constexpr float HISTORY_PLOT_MAX_HEIGHT_EM = 33.75F;

/// Height kept back from the fill so the charts never sum to a hair more than the space they were
/// measured against, which would summon a scrollbar the layout was specifically sized to avoid.
inline constexpr float HISTORY_PLOT_FILL_MARGIN_PX = 2.0F;

/// @param emPx       One em of body text, i.e. ImGui::GetFontSize().
/// @param chartEmPx  One em of the chart's axis and legend text (UI::chartEmPx()); a value
///                   that is not positive and finite means "the same as emPx".
[[nodiscard]] inline float historyPlotMinHeight(float emPx, float chartEmPx = 0.0F) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    const float chartEm = (std::isfinite(chartEmPx) && chartEmPx > 0.0F) ? chartEmPx : em;
    return std::max(HISTORY_PLOT_MIN_HEIGHT_EM * em, HISTORY_PLOT_MIN_HEIGHT_CHART_EM * chartEm);
}

[[nodiscard]] inline float historyPlotMaxHeight(float emPx) noexcept
{
    const float em = (std::isfinite(emPx) && emPx > 0.0F) ? emPx : 1.0F;
    return HISTORY_PLOT_MAX_HEIGHT_EM * em;
}

/// Height for each of `plotCount` charts stacked in a region `availableHeightPx` tall.
///
/// @param emPx               One em, i.e. ImGui::GetFontSize().
/// @param availableHeightPx  Height of the region the charts and everything between them must fit.
/// @param nonPlotHeightPx    Height taken by everything that is not a plot: headings, spacing, the
///                           padding around each chart. Measured from the previous frame.
/// @param plotCount          Number of charts sharing the region.
/// @param chartEmPx          One em of chart text; see historyPlotMinHeight().
/// @return The plot height in whole pixels, within [historyPlotMinHeight, historyPlotMaxHeight]
///         rounded down. With nothing
///         measured yet (plotCount == 0, or an unusable height) it is the minimum, so the first
///         frame under-fills rather than overflowing and the next frame corrects it.
[[nodiscard]] inline float
computeFillPlotHeight(float emPx, float availableHeightPx, float nonPlotHeightPx, std::size_t plotCount, float chartEmPx = 0.0F) noexcept
{
    const float minHeight = std::min(historyPlotMinHeight(emPx, chartEmPx), historyPlotMaxHeight(emPx));
    const float maxHeight = historyPlotMaxHeight(emPx);

    if (plotCount == 0 || !std::isfinite(availableHeightPx) || availableHeightPx <= 0.0F || !std::isfinite(nonPlotHeightPx))
    {
        return std::floor(minHeight);
    }

    const float forPlots = availableHeightPx - std::max(nonPlotHeightPx, 0.0F) - HISTORY_PLOT_FILL_MARGIN_PX;
    const float each = forPlots / static_cast<float>(plotCount);

    // Whole pixels only. ImGui lays items out on whole pixels, so a fractional plot height is
    // rounded differently from one chart to the next, and the caller -- which measures the non-plot
    // height as "what the frame used, less the plots" -- sees that rounding as a change in the
    // non-plot height. That fed back into the next frame: at 2147x1409 on Even Huger the height
    // cycled 233.94 -> 234.69 -> 234.44 -> 234.19 every four frames and the charts visibly
    // shimmered. With whole-pixel heights the layout is exact and the measurement is stable.
    return std::floor(std::clamp(each, minHeight, maxHeight));
}

} // namespace UI::Widgets
