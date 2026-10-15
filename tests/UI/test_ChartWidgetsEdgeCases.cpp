/// @file test_ChartWidgetsEdgeCases.cpp
/// @brief ChartWidgets.h's edge cases (#1395): reductions given nothing to do or samples all at one
/// time, and HistoryChart with a Y axis that doesn't start at 0 and with the Render Metrics overlay on.

#include "UI/ChartWidgets.h"
#include "UI/RenderMetrics.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <implot.h>

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace UI::Widgets
{
namespace
{

constexpr double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

TEST(ChartReductionEdgeTest, NothingToReduceIsNoPoints)
{
    const std::array<double, 3> x{0.0, 1.0, 2.0};
    const std::array<double, 3> y{1.0, 2.0, 3.0};
    std::vector<ReducedPoint> points{ReducedPoint{.index = 9, .gap = false}};
    reduceSeriesMinMaxPoints(x.data(), y.data(), 0, LINE_PLOT_MAX_POINTS_DENSE, 0.0, points);
    EXPECT_TRUE(points.empty()); // cleared, and nothing added
    reduceSeriesMinMaxPoints(x.data(), y.data(), 3, 0, 0.0, points);
    EXPECT_TRUE(points.empty());
}

TEST(ChartReductionEdgeTest, SamplesAllAtOneTimeAreStridedNotBucketed)
{
    // A history whose samples all carry one timestamp (a clock that stood still) has no width to
    // bucket: the series is strided to the budget instead, ends included.
    constexpr int COUNT = 50;
    constexpr int MAX_OUT = 14;
    std::vector<double> x(COUNT, 5.0);
    std::vector<double> y(COUNT);
    for (int i = 0; i < COUNT; ++i)
    {
        y[static_cast<std::size_t>(i)] = static_cast<double>(i);
    }
    std::vector<ReducedPoint> points;
    reduceSeriesMinMaxPoints(x.data(), y.data(), COUNT, MAX_OUT, 0.0, points);
    ASSERT_FALSE(points.empty());
    EXPECT_LE(points.size(), static_cast<std::size_t>(MAX_OUT));
    EXPECT_EQ(points.front().index, 0);
    EXPECT_EQ(points.back().index, COUNT - 1);

    // Fewer samples than the budget: every one is kept.
    reduceSeriesMinMaxPoints(x.data(), y.data(), 5, MAX_OUT, 0.0, points);
    ASSERT_EQ(points.size(), 5U);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_EQ(points[static_cast<std::size_t>(i)].index, i);
        EXPECT_FALSE(points[static_cast<std::size_t>(i)].gap);
    }
}

TEST(ChartReductionEdgeTest, AlignedSamplesAllAtOneTimeAreStridedAndAStepOverAGapIsAGap)
{
    constexpr std::size_t COUNT = 200;
    constexpr int MAX_OUT = 20;
    const std::vector<double> x(COUNT, 7.0);
    std::vector<double> user(COUNT, 10.0);
    std::vector<double> system(COUNT, 5.0);
    for (std::size_t i = 1; i < COUNT; i += 3)
    {
        system[i] = NOT_A_NUMBER; // a missed sample in every stride
    }
    std::vector<ReducedPoint> points;
    reduceAlignedPoints<double>(
        std::span<const double>(x), {std::span<const double>(user), std::span<const double>(system)}, MAX_OUT, 0.0, points);
    ASSERT_EQ(points.size(), static_cast<std::size_t>(MAX_OUT));
    EXPECT_EQ(points.front().index, 0);
    EXPECT_EQ(points.back().index, static_cast<int>(COUNT - 1));
    bool anyGap = false;
    for (std::size_t k = 1; k < points.size(); ++k)
    {
        anyGap = anyGap || points[k].gap; // each step after the first passes over a NaN
    }
    EXPECT_TRUE(anyGap);
    EXPECT_FALSE(points.front().gap);
}

class HistoryChartEdgeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        RenderMetrics::get().setEnabled(false);
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// One frame drawing a history chart from @p config with one series; whether it was active.
    static bool frame(const HistoryChartConfig& config)
    {
        static constexpr std::array<double, 3> X{-2.0, -1.0, 0.0};
        static constexpr std::array<double, 3> Y{20.0, 30.0, 40.0};
        bool active = false;
        ImGui::NewFrame();
        RenderMetrics::get().beginFrame(ImGui::GetFrameCount());
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(800.0F, 600.0F));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        {
            const HistoryChart chart(config);
            active = chart.active();
            if (active)
            {
                plotLineWithFill("Load", X.data(), Y.data(), 3, ImVec4(1, 1, 1, 1));
            }
        }
        ImGui::End();
        ImGui::Render();
        return active;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(HistoryChartEdgeTest, AnAxisNotStartingAtZeroKeepsImPlotsOwnTicks)
{
    // A fixed Y range off zero keeps ImPlot's own ticks rather than the 1-2-5 ones. (A top of 0 or
    // infinity is not tried: ImPlot itself then converts -inf to int, which UBSan rightly rejects, and
    // no chart hands it one -- rateAxisUpperBound() is positive and finite by construction.)
    HistoryChartConfig offZero = percentHistoryConfig("##OffZero", -10.0, 0.0);
    offZero.yLimits = std::pair{10.0, 50.0};
    EXPECT_TRUE(frame(offZero));
}

// #1891: the per-chart caches (each chart's eased Y bound, each series' point reduction) drop what
// hasn't been drawn for a while once they hold many entries, so charts that come and go (per-disk,
// per-interface) don't grow them forever. A chart still being drawn keeps its state.
TEST_F(HistoryChartEdgeTest, ChartCachesPruneChartsNotDrawnForAWhile)
{
    constexpr int CHARTS = 300; // above the caches' 256-entry prune threshold
    const auto inFrame = [](const auto& body)
    {
        ImGui::NewFrame();
        body();
        ImGui::Render();
    };
    inFrame(
        []
        {
            for (int i = 0; i < CHARTS; ++i)
            {
                const auto id = static_cast<ImGuiID>(0xC0DE0000U + static_cast<unsigned>(i));
                static_cast<void>(easedChartUpperBound(id, 100.0));
                static_cast<void>(seriesReductionCache(id, "Load"));
            }
        });
    // Past the caches' 600 stale frames, with one chart still drawn every frame.
    constexpr ImGuiID KEPT = 0xC0DE0001U;
    for (int idle = 0; idle < 610; ++idle)
    {
        inFrame([] { static_cast<void>(easedChartUpperBound(KEPT, 100.0)); });
    }
    double kept = 0.0;
    inFrame(
        [&kept]
        {
            static_cast<void>(seriesReductionCache(0xC0DEFFFFU, "Load")); // a new series: prunes the stale ones
            kept = easedChartUpperBound(KEPT, 10.0);                      // prunes the stale bounds, not this one
        });
    EXPECT_GT(kept, 10.0) << "a chart drawn every frame keeps its bound, easing down rather than jumping";
    EXPECT_LE(kept, 100.0);
}

TEST_F(HistoryChartEdgeTest, RenderMetricsRecordsAHistoryChartWhenOn)
{
    RenderMetrics::get().setEnabled(true);
    const HistoryChartConfig config = percentHistoryConfig("##MeasuredChart", -10.0, 0.0);
    ASSERT_TRUE(frame(config));
    ASSERT_TRUE(frame(config)); // the first frame's samples are published by the next
    bool recorded = false;
    for (const ChartRenderSample& sample : RenderMetrics::get().lastFrame())
    {
        recorded = recorded || (sample.id == "##MeasuredChart" && sample.vertices > 0);
    }
    EXPECT_TRUE(recorded);
}

} // namespace
} // namespace UI::Widgets
