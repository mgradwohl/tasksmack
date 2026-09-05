// Benchmarks for UI/ChartWidgets pure helper functions.
//
// These are the layer the original ETW app-trace flagged as expensive (ImFont::RenderText,
// ImFontCalcTextSizeEx, ImDrawList::AddPolyline, etc. -- see perf-plan-574 / issue #574) but
// that has no Linux-runnable microbenchmark coverage: confirming a text-formatting or
// history-window change actually helped currently requires a full manual Windows ETW capture.
// These functions don't call into the real ImGui/ImPlot runtime (only use their POD types),
// so -- same reasoning as UI/IconLoader.cpp's direct test linkage documented in
// CONTRIBUTING.md -- they benchmark real production code, not a parallel copy.

#include "UI/ChartWidgets.h"

#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <random>
#include <vector>

namespace
{

constexpr size_t kPoolSize = 1024;
constexpr size_t kPoolMask = kPoolSize - 1;

// =============================================================================
// computeAlpha Benchmarks -- called every frame for every smoothed chart value
// (CPU%, memory%, per-core bars, GPU metrics, etc.)
// =============================================================================

static void BM_ChartWidgets_ComputeAlpha(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.001, 0.1); // typical frame delta times
    std::array<double, kPoolSize> deltas{};
    for (auto& v : deltas)
    {
        v = dist(rng);
    }

    const auto refreshInterval = std::chrono::milliseconds(1000);
    size_t idx = 0;
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(UI::Widgets::computeAlpha(deltas[idx & kPoolMask], refreshInterval));
        ++idx;
    }
}
BENCHMARK(BM_ChartWidgets_ComputeAlpha);

// =============================================================================
// tailAlignedSpan Benchmarks -- called once per visible chart per frame to select
// the plotted history window from the full retained buffer.
// =============================================================================

// GPU_HISTORY_CAPACITY-sized buffer (Domain::GPU_HISTORY_CAPACITY = 300, 5 min at 1s
// intervals), requesting the full window -- the common case for a freshly-opened chart.
static void BM_ChartWidgets_TailAlignedSpan_FullWindow(benchmark::State& state)
{
    const std::vector<float> data(300, 42.0F);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(UI::Widgets::tailAlignedSpan(data, data.size()));
    }
}
BENCHMARK(BM_ChartWidgets_TailAlignedSpan_FullWindow);

// Requesting a narrower window than the buffer holds -- the common case once a chart's
// retained history exceeds its currently zoomed/visible time range.
static void BM_ChartWidgets_TailAlignedSpan_PartialWindow(benchmark::State& state)
{
    const auto bufferSize = static_cast<size_t>(state.range(0));
    const auto requestedCount = static_cast<size_t>(state.range(1));
    const std::vector<float> data(bufferSize, 42.0F);

    for (auto _ : state)
    {
        benchmark::DoNotOptimize(UI::Widgets::tailAlignedSpan(data, requestedCount));
    }
}
BENCHMARK(BM_ChartWidgets_TailAlignedSpan_PartialWindow)->Args({300, 60})->Args({1800, 300});

// =============================================================================
// Axis formatter Benchmarks -- called once per visible tick label per axis per
// frame for every open chart (ETW flagged ImFontCalcTextSizeEx-adjacent formatting
// work as a top app-trace hotspot).
// =============================================================================

static void BM_ChartWidgets_FormatAxisLocalized(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 5'000'000'000.0); // spans K/M/G suffix ranges
    std::array<double, kPoolSize> values{};
    for (auto& v : values)
    {
        v = dist(rng);
    }

    char buff[32]{};
    size_t idx = 0;
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(UI::Widgets::formatAxisLocalized(values[idx & kPoolMask], buff, static_cast<int>(sizeof(buff)), nullptr));
        ++idx;
    }
}
BENCHMARK(BM_ChartWidgets_FormatAxisLocalized);

static void BM_ChartWidgets_FormatAxisBytesPerSec(benchmark::State& state)
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> dist(0.0, 5.0 * 1024 * 1024 * 1024); // spans KB/MB/GB per sec
    std::array<double, kPoolSize> values{};
    for (auto& v : values)
    {
        v = dist(rng);
    }

    char buff[32]{};
    size_t idx = 0;
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(
            UI::Widgets::formatAxisBytesPerSec(values[idx & kPoolMask], buff, static_cast<int>(sizeof(buff)), nullptr));
        ++idx;
    }
}
BENCHMARK(BM_ChartWidgets_FormatAxisBytesPerSec);

} // namespace
