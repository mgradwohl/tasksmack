/// @file test_FillPlotLayout.cpp
/// @brief Tests for UI::Widgets::FillPlotLayout's measurement of what a tab spends around its first
/// chart (PlotFillState::firstPlot), under a live ImGui context, and for the minimum
/// window height's first-chart budget built from it (#1370 review).

#include "App/TitleBarGeometry.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/HistoryPlotHeight.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace UI::Widgets
{
namespace
{

/// One entry of the GPU core chart's value strip, as GpuSection draws it.
struct StripEntry
{
    std::string_view head;
    std::string_view tail;
};

constexpr std::array GPU_CORE_STRIP{
    StripEntry{.head = "Utilization", .tail = "6.1%"},
    StripEntry{.head = "Memory", .tail = "6.9% (1.2 GiB / 17.9 GiB)"},
    StripEntry{.head = "Clock", .tail = "1450 MHz (63%)"},
    StripEntry{.head = "Encoder", .tail = "0.0%"},
    StripEntry{.head = "Decoder", .tail = "0.0%"},
};

class FillPlotLayoutTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(2400.0F, 1600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a GPU-tab-like body @p width wide: its title, a Spacing(), the first chart's
    /// heading and value strip (wrapping as GpuSection's does, Detail::drawValueStripEntry()), the
    /// chart, then a second chart.
    static void renderGpuLikeTab(float width, PlotFillState& state)
    {
        // The window is the whole viewport, as the app's is: its width is what a measurement records.
        ImGui::GetIO().DisplaySize.x = width;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width, 1400.0F));
        ImGui::Begin("Tab", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        {
            FillPlotLayout fill(state);
            ImGui::TextUnformatted("GPU Monitoring (1 GPU)");
            ImGui::Spacing();
            ImGui::TextUnformatted("GPU Core & Video (256 samples)");
            const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const ImVec4 color(0.5F, 0.4F, 0.8F, 1.0F);
            const ImVec4 muted(0.6F, 0.6F, 0.6F, 1.0F);
            bool first = true;
            for (const StripEntry& entry : GPU_CORE_STRIP)
            {
                Detail::drawValueStripEntry(entry.head, entry.tail, color, first, true, rowRight, muted);
                first = false;
            }
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, fill.plotHeight()));
            fill.addPlot();
            ImGui::TextUnformatted("Thermal & Power");
            ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, fill.plotHeight()));
            fill.addPlot();
        }
        ImGui::End();
        ImGui::EndFrame();
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

// The measurement is exactly what the tab spent down to the end of its first chart, less its plot:
// with room for the strip on one row, title, Spacing(), heading, one strip row and the spacing after
// the chart. The second chart is not counted.
TEST_F(FillPlotLayoutTest, MeasuresEverythingAroundTheFirstChartButItsPlot)
{
    PlotFillState state;
    renderGpuLikeTab(2000.0F, state);
    renderGpuLikeTab(2000.0F, state);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeight();
    const float spacing = style.ItemSpacing.y;
    // title + Spacing() + heading + strip row, then the spacing after the plot
    EXPECT_FLOAT_EQ(state.firstPlot.nonPlotHeight, (3.0F * (line + spacing)) + spacing + spacing);
    EXPECT_EQ(state.plotCount, 2U);
}

// Copilot on #1370: in a narrow window the GPU chart's value strip wraps onto extra rows, which the
// estimated first-chart block (computeTallestFirstChartBlock()) does not count, so at the minimum
// height the first plot did not fit. The measured height carries every wrapped row, and the
// budget the minimum height is built from takes it over the estimate.
TEST_F(FillPlotLayoutTest, WrappedValueStripRowsReachTheMinimumHeightBudget)
{
    PlotFillState wide;
    renderGpuLikeTab(2000.0F, wide);
    renderGpuLikeTab(2000.0F, wide);
    PlotFillState narrow;
    renderGpuLikeTab(260.0F, narrow);
    renderGpuLikeTab(260.0F, narrow);

    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineWithSpacing = ImGui::GetTextLineHeight() + style.ItemSpacing.y;
    // At 260px the five entries take at least three rows where the wide tab used one.
    EXPECT_GE(narrow.firstPlot.nonPlotHeight, wide.firstPlot.nonPlotHeight + (2.0F * lineWithSpacing));

    const float plotMin = std::floor(historyPlotMinHeight(ImGui::GetFontSize()));
    const float estimate = App::computeTallestFirstChartBlock({
        .textLineWithSpacingPx = lineWithSpacing,
        .frameHeightWithSpacingPx = ImGui::GetFrameHeightWithSpacing(),
        .itemSpacingYPx = style.ItemSpacing.y,
        .cellPaddingYPx = style.CellPadding.y,
        .plotMinHeightPx = plotMin,
    });
    const float budget = App::computeFirstChartBudget(estimate, narrow.firstPlot.nonPlotHeight, plotMin);
    EXPECT_GT(budget, estimate);
    EXPECT_FLOAT_EQ(budget, narrow.firstPlot.nonPlotHeight + plotMin);
}

// A tab that drew no chart this frame measures nothing, and the budget falls back to the estimate.
TEST_F(FillPlotLayoutTest, NoChartMeasuresNothing)
{
    PlotFillState state;
    state.firstPlot.nonPlotHeight = 123.0F;
    ImGui::NewFrame();
    ImGui::Begin("Empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    {
        const FillPlotLayout fill(state);
        ImGui::TextUnformatted("No GPUs");
    }
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_FLOAT_EQ(state.firstPlot.nonPlotHeight, 0.0F);
    EXPECT_FLOAT_EQ(App::computeFirstChartBudget(250.0F, state.firstPlot.nonPlotHeight, 120.0F), 250.0F);
}

// Copilot on #1370: a tab measures only while it is shown, so its figure must not outlive the layout
// it was taken under. Measure the GPU tab with its strip wrapped in a narrow window, then -- with
// that tab hidden -- widen the window or change the font: the old, taller figure no longer counts,
// and the budget falls back to the estimate (or the shown tab's own current measurement).
TEST_F(FillPlotLayoutTest, HiddenTabsMeasurementGoesStaleWhenTheLayoutChanges)
{
    PlotFillState gpu;
    renderGpuLikeTab(260.0F, gpu);
    renderGpuLikeTab(260.0F, gpu);
    const float fontSize = ImGui::GetFontSize();
    const float wrapped = gpu.firstPlot.nonPlotHeight;
    ASSERT_GT(wrapped, 0.0F);
    EXPECT_FLOAT_EQ(gpu.firstPlot.windowWidth, 260.0F);
    EXPECT_FLOAT_EQ(gpu.firstPlot.fontSize, fontSize);

    // Still the layout it was measured in: it counts.
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(gpu.firstPlot, 260.0F, fontSize), wrapped);

    // Another tab is shown while the window is widened; the GPU tab is not drawn again.
    PlotFillState overview;
    renderGpuLikeTab(2000.0F, overview);
    renderGpuLikeTab(2000.0F, overview);
    EXPECT_FLOAT_EQ(gpu.firstPlot.windowWidth, 260.0F); // untouched while hidden
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(gpu.firstPlot, 2000.0F, fontSize), 0.0F);
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(overview.firstPlot, 2000.0F, fontSize), overview.firstPlot.nonPlotHeight);

    // The budget at the new width is the shown tab's, not the stale wrapped GPU figure.
    const float shownMax = std::max(currentFirstPlotNonPlotHeight(gpu.firstPlot, 2000.0F, fontSize),
                                    currentFirstPlotNonPlotHeight(overview.firstPlot, 2000.0F, fontSize));
    EXPECT_LT(shownMax, wrapped);
    EXPECT_FLOAT_EQ(App::computeFirstChartBudget(150.0F, shownMax, 120.0F), std::max(150.0F, shownMax + 120.0F));

    // Narrower than it was measured at also no longer describes it (the strip may wrap further).
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(gpu.firstPlot, 240.0F, fontSize), 0.0F);
    // A font change at the same width, likewise.
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(gpu.firstPlot, 260.0F, fontSize + 2.0F), 0.0F);
}

TEST(FirstPlotMeasurementTest, OnlyAMeasurementOfTheCurrentLayoutCounts)
{
    const FirstPlotMeasurement measured{.nonPlotHeight = 90.0F, .windowWidth = 748.0F, .fontSize = 13.0F};
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 748.0F, 13.0F), 90.0F);
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 748.4F, 13.0F), 90.0F); // within tolerance
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 749.0F, 13.0F), 0.0F);
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 747.0F, 13.0F), 0.0F);
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 748.0F, 16.0F), 0.0F);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, nan, 13.0F), 0.0F);
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(measured, 748.0F, nan), 0.0F);
    // Never measured: nothing, even at the default (0, 0) layout.
    EXPECT_FLOAT_EQ(currentFirstPlotNonPlotHeight(FirstPlotMeasurement{}, 0.0F, 0.0F), 0.0F);
}

} // namespace
} // namespace UI::Widgets
