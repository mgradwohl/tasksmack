/// @file test_ChartGeometryBudget.cpp
/// @brief Holds the real charts' draw geometry to a budget: renders benchmarks/ChartGeometryScenes.h's
/// scenes headless and fails if a frame's vertex or index count grows past it (#1421, #843 Phase 3's
/// vertex-pressure checks).
///
/// Timing on shared CI runners is too noisy to gate every PR, but geometry is not: a change that
/// doubles a chart's vertices (a lost point cap, a fill drawn twice, AA geometry on a path that had
/// none) fails here deterministically. bench_ChartGeometry.cpp times the same frames.
///
/// Each budget is the count recorded when it was set plus 25 %. The counts move by a percent or two
/// between runs (the reduction buckets are anchored in wall-clock time, see ChartGeometryScenes.h),
/// well inside that margin. A deliberate geometry increase raises the budget in the same change, with
/// the reason; a geometry reduction should lower it, so the saving is kept. The floor (half the
/// recorded count) catches a scene that silently stopped drawing, which would otherwise pass any
/// budget.

#include "ChartGeometryScenes.h"
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>

namespace
{

struct GeometryBudget
{
    ChartGeometry::Scene scene;
    int recordedVertices; ///< Measured when the budget was set (2026-10, ImGui 1.92 / ImPlot 1.0)
    int recordedIndices;
};

/// Budget = recorded * 1.25; floor = recorded / 2.
constexpr int budgetFor(int recorded)
{
    return recorded + (recorded / 4);
}

constexpr int floorFor(int recorded)
{
    return recorded / 2;
}

constexpr std::array BUDGETS{
    GeometryBudget{.scene = ChartGeometry::Scene::CpuStacked, .recordedVertices = 12'600, .recordedIndices = 17'600},
    GeometryBudget{.scene = ChartGeometry::Scene::PerCoreSparklines, .recordedVertices = 45'550, .recordedIndices = 61'450},
    GeometryBudget{.scene = ChartGeometry::Scene::Memory, .recordedVertices = 6'300, .recordedIndices = 9'200},
    GeometryBudget{.scene = ChartGeometry::Scene::LongSeriesMinMax, .recordedVertices = 3'950, .recordedIndices = 5'300},
};

// The recorded counts were taken at these scene sizes, which ChartGeometryScenes.h derives from
// SamplingConfig.h. If a sampling limit changes, the scenes draw a different history: re-record the
// counts above at the new sizes, then update these.
static_assert(ChartGeometry::FULL_HISTORY_SAMPLES == 18'000, "scene size changed: re-record the geometry budgets");
static_assert(ChartGeometry::DEFAULT_WINDOW_SAMPLES == 3'000, "scene size changed: re-record the geometry budgets");
static_assert(Domain::Sampling::REFRESH_INTERVAL_MIN_MS == 100, "scene cadence changed: re-record the geometry budgets");

static_assert(BUDGETS.size() == ChartGeometry::ALL_SCENES.size(), "every scene needs a budget");

/// The scene data, built once for every test here.
const ChartGeometry::SceneData& sceneData()
{
    static const ChartGeometry::SceneData k_Data;
    return k_Data;
}

// Every scene's settled frame stays within its vertex and index budget, and above its floor. One
// plain test over all scenes (no parameterized suite: CodeQL flags the statics
// INSTANTIATE_TEST_SUITE_P generates, see #1391), with each scene named in its failures.
TEST(ChartGeometryBudgetTest, EverySceneStaysWithinVertexAndIndexBudget)
{
    for (const GeometryBudget& budget : BUDGETS)
    {
        const char* name = ChartGeometry::sceneName(budget.scene);
        SCOPED_TRACE(name);
        const ChartGeometry::HeadlessChartContext context;
        ChartGeometry::SceneState state;
        const ChartGeometry::FrameGeometry geometry = ChartGeometry::renderSettledFrame(budget.scene, sceneData(), state);

        RecordProperty(std::string(name) + "_vertices", geometry.vertices);
        RecordProperty(std::string(name) + "_indices", geometry.indices);
        EXPECT_LE(geometry.vertices, budgetFor(budget.recordedVertices))
            << name << ": " << geometry.vertices << " vertices against a budget of " << budgetFor(budget.recordedVertices) << " (recorded "
            << budget.recordedVertices << " + 25%)";
        EXPECT_LE(geometry.indices, budgetFor(budget.recordedIndices))
            << name << ": " << geometry.indices << " indices against a budget of " << budgetFor(budget.recordedIndices) << " (recorded "
            << budget.recordedIndices << " + 25%)";
        EXPECT_GE(geometry.vertices, floorFor(budget.recordedVertices))
            << name << ": only " << geometry.vertices << " vertices; did the scene stop drawing its charts? If the geometry was cut "
            << "on purpose, lower recordedVertices";
        EXPECT_GE(geometry.indices, floorFor(budget.recordedIndices))
            << name << ": only " << geometry.indices << " indices; did the scene stop drawing its charts? If the geometry was cut "
            << "on purpose, lower recordedIndices";
    }
}

// The same fixed scene renders the same geometry frame after frame: the counts the budgets hold are
// a property of the charts, not of how many frames have run (only the wall-clock reduction anchor,
// see ChartGeometryScenes.h, may move a few points).
TEST(ChartGeometryScenesTest, SettledFramesRepeatTheirGeometry)
{
    const ChartGeometry::HeadlessChartContext context;
    ChartGeometry::SceneState state;
    const ChartGeometry::FrameGeometry first = ChartGeometry::renderSettledFrame(ChartGeometry::Scene::Memory, sceneData(), state);
    const ChartGeometry::FrameGeometry second = ChartGeometry::renderFrame(ChartGeometry::Scene::Memory, sceneData(), state);

    ASSERT_GT(first.vertices, 0);
    const int tolerance = std::max(1, first.vertices / 50); // 2 %
    EXPECT_NEAR(second.vertices, first.vertices, tolerance);
    EXPECT_EQ(second.drawLists, first.drawLists);
}

} // namespace
