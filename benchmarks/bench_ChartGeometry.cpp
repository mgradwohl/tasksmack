// Headless ImGui + ImPlot geometry benchmarks for the real UI/ChartWidgets.h charts (#1421).
//
// bench_UI.cpp covers ChartWidgets' pure helpers only. The costs the Windows traces in #843 found
// dominant -- ImDrawList::AddPolyline, ImPlot::PlotShadedEx, ImFont::RenderText, draw-list
// submission -- happen inside the ImGui/ImPlot runtime, which these benchmarks run: each iteration
// builds and renders one whole frame of a chart scene (ImGui::NewFrame() through ImGui::Render()),
// with no window, renderer or OpenGL. See ChartGeometryScenes.h for the scenes and how they stay
// deterministic.
//
// Time is the frame's CPU cost: widget code, ImPlot's item rendering and draw-list generation.
// The counters report what that frame produced -- `vertices`, `indices`, `draw_lists`, `draw_cmds`
// from ImGui::GetDrawData() -- so a timing change can be read against a geometry change. The same
// scenes' counts are held to a budget by tests/UI/test_ChartGeometryBudget.cpp, which (unlike these
// timings) is stable enough to gate every PR.

#include "ChartGeometryScenes.h"

#include <benchmark/benchmark.h>

namespace
{

void runScene(benchmark::State& state, ChartGeometry::Scene scene)
{
    // Built once per process: the data is fixed, and building 18k-sample series is not what is measured.
    static const ChartGeometry::SceneData data;
    const ChartGeometry::HeadlessChartContext context;
    ChartGeometry::SceneState sceneState;
    ChartGeometry::FrameGeometry geometry = ChartGeometry::renderSettledFrame(scene, data, sceneState);

    for (auto _ : state)
    {
        geometry = ChartGeometry::renderFrame(scene, data, sceneState);
        benchmark::DoNotOptimize(geometry);
    }

    state.counters["vertices"] = benchmark::Counter(static_cast<double>(geometry.vertices));
    state.counters["indices"] = benchmark::Counter(static_cast<double>(geometry.indices));
    state.counters["draw_lists"] = benchmark::Counter(static_cast<double>(geometry.drawLists));
    state.counters["draw_cmds"] = benchmark::Counter(static_cast<double>(geometry.commands));
}

// The Overview CPU chart at full history (18k samples): three bands reduced together and cached per
// data generation, their edges with markers, the Total line, and four now bars with a value strip.
void BM_ChartGeometry_CpuStacked_FullHistory(benchmark::State& state)
{
    runScene(state, ChartGeometry::Scene::CpuStacked);
}
BENCHMARK(BM_ChartGeometry_CpuStacked_FullHistory)->Unit(benchmark::kMicrosecond);

// The CPU Cores grid: 16 small filled charts (3k samples each) with a now bar apiece.
void BM_ChartGeometry_PerCoreSparklines(benchmark::State& state)
{
    runScene(state, ChartGeometry::Scene::PerCoreSparklines);
}
BENCHMARK(BM_ChartGeometry_PerCoreSparklines)->Unit(benchmark::kMicrosecond);

// The CPU Cores grid on a 64-thread machine: 64 narrow filled charts (3k samples each), whose point
// budget follows their plot width (#1411).
void BM_ChartGeometry_ManyCoreSparklines(benchmark::State& state)
{
    runScene(state, ChartGeometry::Scene::ManyCoreSparklines);
}
BENCHMARK(BM_ChartGeometry_ManyCoreSparklines)->Unit(benchmark::kMicrosecond);

// The Memory chart: Used (filled), Cached and Swap with markers, a peak line, three now bars (3k samples).
void BM_ChartGeometry_Memory(benchmark::State& state)
{
    runScene(state, ChartGeometry::Scene::Memory);
}
BENCHMARK(BM_ChartGeometry_Memory)->Unit(benchmark::kMicrosecond);

// One filled 18k-sample line in a chart with no data generation: plotLineWithFill() min/max-reduces
// the whole history on every frame, the uncached path #1139 avoids elsewhere.
void BM_ChartGeometry_LongSeriesMinMax_FullHistory(benchmark::State& state)
{
    runScene(state, ChartGeometry::Scene::LongSeriesMinMax);
}
BENCHMARK(BM_ChartGeometry_LongSeriesMinMax_FullHistory)->Unit(benchmark::kMicrosecond);

} // namespace
