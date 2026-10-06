/// @file test_FillPlotLayout.cpp
/// @brief Tests for UI::Widgets::FillPlotLayout's measurement of what a tab spends around its first
/// chart (PlotFillState::firstPlotNonPlotHeight), under a live ImGui context, and for the minimum
/// window height's first-chart budget built from it (#1370 review).

#include "App/TitleBarGeometry.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"
#include "UI/HistoryPlotHeight.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <array>
#include <cmath>
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
    EXPECT_FLOAT_EQ(state.firstPlotNonPlotHeight, (3.0F * (line + spacing)) + spacing + spacing);
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
    EXPECT_GE(narrow.firstPlotNonPlotHeight, wide.firstPlotNonPlotHeight + (2.0F * lineWithSpacing));

    const float plotMin = std::floor(historyPlotMinHeight(ImGui::GetFontSize()));
    const float estimate = App::computeTallestFirstChartBlock({
        .textLineWithSpacingPx = lineWithSpacing,
        .frameHeightWithSpacingPx = ImGui::GetFrameHeightWithSpacing(),
        .itemSpacingYPx = style.ItemSpacing.y,
        .cellPaddingYPx = style.CellPadding.y,
        .plotMinHeightPx = plotMin,
    });
    const float budget = App::computeFirstChartBudget(estimate, narrow.firstPlotNonPlotHeight, plotMin);
    EXPECT_GT(budget, estimate);
    EXPECT_FLOAT_EQ(budget, narrow.firstPlotNonPlotHeight + plotMin);
}

// A tab that drew no chart this frame measures nothing, and the budget falls back to the estimate.
TEST_F(FillPlotLayoutTest, NoChartMeasuresNothing)
{
    PlotFillState state;
    state.firstPlotNonPlotHeight = 123.0F;
    ImGui::NewFrame();
    ImGui::Begin("Empty", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    {
        const FillPlotLayout fill(state);
        ImGui::TextUnformatted("No GPUs");
    }
    ImGui::End();
    ImGui::EndFrame();
    EXPECT_FLOAT_EQ(state.firstPlotNonPlotHeight, 0.0F);
    EXPECT_FLOAT_EQ(App::computeFirstChartBudget(250.0F, state.firstPlotNonPlotHeight, 120.0F), 250.0F);
}

} // namespace
} // namespace UI::Widgets
