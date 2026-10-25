/// @file test_ChartFrameCaches.cpp
/// @brief The chart caches that keep per-publication work out of the frames between samples (#843),
/// in headless ImGui frames: a window's series maximum is scanned once per data generation and window
/// start (windowSeriesMax()), not every frame, and the stacked CPU chart (benchmarks/ChartGeometryScenes.h
/// draws it as SystemMetricsPanel does) gathers its band edges once per choice of points, rebuilding
/// only their x every frame.

#include "ChartGeometryScenes.h"
#include "UI/ChartWidgets.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace
{

/// The scene data, built once for every test here.
const ChartGeometry::SceneData& sceneData()
{
    static const ChartGeometry::SceneData k_Data;
    return k_Data;
}

/// What one frame's windowSeriesMax() call returned, and how often its cache had scanned by then.
struct WindowMaxFrame
{
    double value = 0.0;
    std::uint64_t computeCount = 0;
};

/// One headless frame in which an axis named "##Axis" asks for the maximum of @p series in the last
/// 60 s of @p x under @p generation.
WindowMaxFrame windowMaxFrame(std::uint64_t generation, std::span<const double> x, std::span<const float> series)
{
    WindowMaxFrame result;
    ImGui::NewFrame();
    ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoSavedSettings);
    result.value = UI::Widgets::windowSeriesMax("##Axis", generation, x, -60.0, series);
    result.computeCount = UI::Widgets::visibleMaxCache("##Axis").computeCount();
    ImGui::End();
    ImGui::Render();
    return result;
}

TEST(ChartFrameCachesTest, AWindowMaxIsScannedOncePerGenerationNotEveryFrame)
{
    const ChartGeometry::HeadlessChartContext context;
    const std::array<double, 3> x{-50.0, -20.0, -1.0};
    const std::array<float, 3> series{5.0F, 1.0F, 2.0F};
    const std::uint64_t generation = UI::Widgets::nextChartDataGeneration();

    const WindowMaxFrame first = windowMaxFrame(generation, x, series);
    EXPECT_DOUBLE_EQ(first.value, 5.0);
    EXPECT_EQ(first.computeCount, 1U);
    // The frames between samples replay it.
    for (int frame = 0; frame < 3; ++frame)
    {
        const WindowMaxFrame between = windowMaxFrame(generation, x, series);
        EXPECT_DOUBLE_EQ(between.value, 5.0);
        EXPECT_EQ(between.computeCount, 1U);
    }
    // A new publication's frame scans again.
    const WindowMaxFrame next = windowMaxFrame(UI::Widgets::nextChartDataGeneration(), x, series);
    EXPECT_DOUBLE_EQ(next.value, 5.0);
    EXPECT_EQ(next.computeCount, 2U);
}

TEST(ChartFrameCachesTest, AxesInOtherIdScopesKeepTheirOwnMaximum)
{
    // Per-disk and per-GPU charts share their axis names under their own PushID() scopes.
    const ChartGeometry::HeadlessChartContext context;
    const std::array<double, 2> x{-10.0, -1.0};
    const std::array<float, 2> diskA{3.0F, 4.0F};
    const std::array<float, 2> diskB{8.0F, 1.0F};
    const std::uint64_t generation = UI::Widgets::nextChartDataGeneration();
    for (int frame = 0; frame < 2; ++frame)
    {
        ImGui::NewFrame();
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushID("Disk A");
        EXPECT_DOUBLE_EQ(UI::Widgets::windowSeriesMax("##DiskAxis", generation, x, -60.0, diskA), 4.0);
        EXPECT_EQ(UI::Widgets::visibleMaxCache("##DiskAxis").computeCount(), 1U);
        ImGui::PopID();
        ImGui::PushID("Disk B");
        EXPECT_DOUBLE_EQ(UI::Widgets::windowSeriesMax("##DiskAxis", generation, x, -60.0, diskB), 8.0);
        EXPECT_EQ(UI::Widgets::visibleMaxCache("##DiskAxis").computeCount(), 1U);
        ImGui::PopID();
        ImGui::End();
        ImGui::Render();
    }
}

TEST(ChartFrameCachesTest, TheStackedChartGathersItsBandEdgesOncePerGeneration)
{
    const ChartGeometry::HeadlessChartContext context;
    ChartGeometry::SceneState state;
    // Settled first: ImPlot's layout moves the plot's width, and so its point budget, over the first
    // frames, and a new budget is a new choice of points.
    static_cast<void>(ChartGeometry::renderSettledFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    const std::uint64_t reductions = state.cpuStackReduction.rebuildCount();
    const std::uint64_t gathers = state.cpuStackValues.gatherCount();
    EXPECT_EQ(gathers, reductions); // gathered once for each choice of points, never more

    // The frames between samples: the same points and values; only x is rebuilt.
    for (int frame = 0; frame < 5; ++frame)
    {
        static_cast<void>(ChartGeometry::renderFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    }
    EXPECT_EQ(state.cpuStackReduction.rebuildCount(), reductions);
    EXPECT_EQ(state.cpuStackValues.gatherCount(), gathers);

    // A new publication: reduced and gathered again, once.
    state.dataGeneration = UI::Widgets::nextChartDataGeneration();
    static_cast<void>(ChartGeometry::renderFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    static_cast<void>(ChartGeometry::renderFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    EXPECT_EQ(state.cpuStackReduction.rebuildCount(), reductions + 1);
    EXPECT_EQ(state.cpuStackValues.gatherCount(), gathers + 1);
}

TEST(ChartFrameCachesTest, AStackedChartWhoseCacheIsResetGathersItsBandEdgesAgain)
{
    // The points cache reset between two frames under the same generation (a panel rebuilt, an entry
    // pruned and made again) counts its rebuilds from 0 again; the band edges must still be gathered
    // for its new points, each as long as their x or one longer, never shorter (#843 review).
    const ChartGeometry::HeadlessChartContext context;
    ChartGeometry::SceneState state;
    static_cast<void>(ChartGeometry::renderSettledFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    const std::uint64_t gathers = state.cpuStackValues.gatherCount();

    state.cpuStackReduction = UI::Widgets::ReducedPointsCache{};
    static_cast<void>(ChartGeometry::renderFrame(ChartGeometry::Scene::CpuStacked, sceneData(), state));
    EXPECT_EQ(state.cpuStackValues.gatherCount(), gathers + 1);
    for (const std::vector<double>* edge :
         {&state.cpuStack.base, &state.cpuStack.userTop, &state.cpuStack.systemTop, &state.cpuIowaitTop, &state.cpuBusyTop})
    {
        EXPECT_GE(edge->size(), state.cpuStack.x.size());
        EXPECT_LE(edge->size(), state.cpuStack.x.size() + 1);
    }
}

} // namespace
