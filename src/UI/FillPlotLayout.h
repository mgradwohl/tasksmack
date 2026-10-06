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
#include <cstddef>

namespace UI::Widgets
{

/// What one tab remembers between frames for FillPlotLayout. Keep one per tab, not one per panel:
/// tabs differ in how many charts they hold and how much else is on them.
struct PlotFillState
{
    float nonPlotHeight = 0.0F; ///< Height the tab spent on everything that is not a plot.
    std::size_t plotCount = 0;  ///< Charts rendered; 0 until the tab has rendered once.
    /// Height the tab spent, from its top to the end of its first chart, on everything but that
    /// chart's plot: whatever is above it, its heading, its value strip on however many rows it
    /// wrapped to, the table's padding and the spacing after it. Measured, so it is right at any
    /// width; the window's minimum height is built from it (#1370 review). 0 until a chart rendered.
    float firstPlotNonPlotHeight = 0.0F;
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
        m_State.firstPlotNonPlotHeight = (m_PlotCount > 0) ? std::max(m_FirstPlotEnd - m_PlotHeight, 0.0F) : 0.0F;
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
    /// first call can note where the first chart ends (PlotFillState::firstPlotNonPlotHeight).
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
