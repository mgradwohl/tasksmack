#pragma once

// The ImGui half of the shared history-chart height rule (UI/HistoryPlotHeight.h has the arithmetic).
//
// A tab that stacks several history charts wants them to share the height it has. The part of that
// height which is *not* plot -- headings, spacing, the padding around each chart, any block above
// the charts -- cannot be known before it is laid out, so it is measured as the tab renders and used
// on the next frame. This is that measure-then-fill step, so each tab does not reimplement it.

#include "UI/HistoryPlotHeight.h"
#include "UI/Theme.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace UI::Widgets
{

/// What a tab spent around its first chart, and the layout it was measured under (#1370 review).
struct FirstPlotMeasurement
{
    /// Height the tab spent, from its top to the end of its first chart, on everything but that
    /// chart's plot: whatever is above it, its heading, its value strip on however many rows it
    /// wrapped to, the table's padding and the spacing after it. 0 until a chart rendered.
    float nonPlotHeight = 0.0F;
    float windowWidth = 0.0F; ///< The main viewport's width it was measured at.
    float fontSize = 0.0F;    ///< ImGui::GetFontSize() it was measured at.
};

/// Widest difference in window width, in pixels, at which a measurement still describes the layout.
inline constexpr float FIRST_PLOT_MEASUREMENT_WIDTH_TOLERANCE_PX = 0.5F;
/// Likewise for the font size (it moves in whole preset steps, so any real change is far larger).
inline constexpr float FIRST_PLOT_MEASUREMENT_FONT_TOLERANCE_PX = 0.01F;

/// @p measurement's non-plot height if it was taken under the current layout -- the same window
/// width and font size -- else 0, "not known". A tab measures only while it is shown, so a hidden
/// tab's figure goes stale when the window is resized or the font changes: wider, its value strip
/// may have unwrapped and the old height is too tall; narrower, it may wrap further and the old
/// height is too short. Either way it no longer describes the tab, and the caller falls back to its
/// estimate until the tab is shown again (#1370 review).
[[nodiscard]] inline float
currentFirstPlotNonPlotHeight(const FirstPlotMeasurement& measurement, const float windowWidth, const float fontSize) noexcept
{
    const bool sameWidth =
        std::isfinite(windowWidth) && std::abs(measurement.windowWidth - windowWidth) <= FIRST_PLOT_MEASUREMENT_WIDTH_TOLERANCE_PX;
    const bool sameFont = std::isfinite(fontSize) && std::abs(measurement.fontSize - fontSize) <= FIRST_PLOT_MEASUREMENT_FONT_TOLERANCE_PX;
    const bool usable = std::isfinite(measurement.nonPlotHeight) && measurement.nonPlotHeight > 0.0F;
    return (sameWidth && sameFont && usable) ? measurement.nonPlotHeight : 0.0F;
}

/// What one tab remembers between frames for FillPlotLayout. Keep one per tab, not one per panel:
/// tabs differ in how many charts they hold and how much else is on them.
struct PlotFillState
{
    float nonPlotHeight = 0.0F; ///< Height the tab spent on everything that is not a plot.
    std::size_t plotCount = 0;  ///< Charts rendered; 0 until the tab has rendered once.
    /// What the tab spent around its first chart, measured as it was last drawn: the window's
    /// minimum height is built from it while it is current (currentFirstPlotNonPlotHeight()).
    FirstPlotMeasurement firstPlot;
};

/// Scope for one frame of a tab whose charts share its height.
///
/// Construct it at the top of the tab's content, give every chart plotHeight(), call addPlot() once
/// per chart rendered, and let it go out of scope at the end of the content. The destructor measures
/// what the frame spent on non-plot content and stores it in the state for the next frame. That
/// figure does not depend on the plot height (heights are whole pixels), so it settles in a frame.
///
/// Charts that are skipped (no data yet, capability absent) simply do not call addPlot(); the next
/// frame's height accounts for however many were actually drawn.
class FillPlotLayout
{
  public:
    /// @param state           The tab's measurements from the previous frame.
    /// @param reservedShares  Shares of the height kept for content rendered *after* this scope
    ///                        ends that takes whatever is left (the per-disk grid on Network and
    ///                        I/O). They count when the height is divided but are never measured,
    ///                        so content that fills the remainder cannot feed back into the next
    ///                        frame's division.
    /// @param reservedMinHeightPx  Least height that content needs, however few shares it is
    ///                        given (the grid's rows at their minimum, #1370 review); see
    ///                        computeFillPlotHeightWithReserve().
    explicit FillPlotLayout(PlotFillState& state, std::size_t reservedShares = 0, float reservedMinHeightPx = 0.0F)
        : m_State(state),
          m_Top(ImGui::GetCursorPosY()),
          m_PlotHeight(computeFillPlotHeightWithReserve(ImGui::GetFontSize(),
                                                        ImGui::GetContentRegionAvail().y,
                                                        state.nonPlotHeight,
                                                        // Nothing measured yet stays nothing: the first
                                                        // frame under-fills (see computeFillPlotHeight()).
                                                        state.plotCount,
                                                        reservedShares,
                                                        reservedMinHeightPx,
                                                        chartEmPx()))
    {}

    ~FillPlotLayout()
    {
        const float used = ImGui::GetCursorPosY() - m_Top;
        m_State.nonPlotHeight = used - (static_cast<float>(m_PlotCount) * m_PlotHeight);
        m_State.plotCount = m_PlotCount;
        m_State.firstPlot = FirstPlotMeasurement{
            .nonPlotHeight = (m_PlotCount > 0) ? std::max(m_FirstPlotEnd - m_PlotHeight, 0.0F) : 0.0F,
            .windowWidth = ImGui::GetMainViewport()->Size.x,
            .fontSize = ImGui::GetFontSize(),
        };
    }

    FillPlotLayout(const FillPlotLayout&) = delete;
    FillPlotLayout& operator=(const FillPlotLayout&) = delete;
    FillPlotLayout(FillPlotLayout&&) = delete;
    FillPlotLayout& operator=(FillPlotLayout&&) = delete;

    /// Height every chart in this tab should use this frame, in whole pixels.
    [[nodiscard]] float plotHeight() const noexcept
    {
        return m_PlotHeight;
    }

    /// Record that a chart of plotHeight() was rendered. Call it straight after the chart, so the
    /// first call can note where the first chart ends (PlotFillState::firstPlot).
    void addPlot() noexcept
    {
        if (m_PlotCount == 0)
        {
            m_FirstPlotEnd = ImGui::GetCursorPosY() - m_Top;
        }
        ++m_PlotCount;
    }

  private:
    PlotFillState& m_State;
    float m_Top;
    float m_PlotHeight;
    std::size_t m_PlotCount = 0;
    float m_FirstPlotEnd = 0.0F; ///< Where the first chart ended, from m_Top.
};

} // namespace UI::Widgets
