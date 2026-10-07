// Benchmarks for Process Details' per-process history (App/Panels/ProcessDetailsHistory.h, #1179).
//
// The panel appends one point to the time axis and every series per sample, then trims to the
// history window. The benchmark runs that pair at a steady, full window: the history is filled to the
// window first, so every timed iteration both appends and drops the oldest point, the steady state a
// selected process stays in. Front-erasing every buffer on each trim made a sample cost the whole
// window in memmove; trimming lazily makes it amortized O(1).
//
// The window and sampling interval come from Domain::Sampling, so the cases follow the settings'
// real bounds: the default window and the longest, both at the fastest refresh interval.
//
// The history object goes through benchmark::DoNotOptimize so the stored values are not treated as
// dead (see bench_History.cpp, #877).

#include "App/Panels/ProcessDetailsHistory.h"
#include "Domain/SamplingConfig.h"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

namespace
{

/// A point with every series set, so each push stores a distinct value per series.
[[nodiscard]] App::Detail::ProcessHistoryPoint pointAt(double value) noexcept
{
    App::Detail::ProcessHistoryPoint point;
    for (std::size_t i = 0; i < App::Detail::PROCESS_SERIES_COUNT; ++i)
    {
        point.*App::Detail::PROCESS_SERIES_FIELDS[i] = value + static_cast<double>(i);
    }
    return point;
}

// state.range(0): the history window in seconds; state.range(1): the sampling interval in ms.
void BM_ProcessDetailsHistory_AppendTrimFullWindow(benchmark::State& state)
{
    const auto windowSeconds = static_cast<double>(state.range(0));
    const double intervalSeconds = static_cast<double>(state.range(1)) / 1000.0;
    const auto samplesPerWindow = static_cast<std::int64_t>(windowSeconds / intervalSeconds);

    App::Detail::ProcessDetailsHistory history;
    std::int64_t sample = 0;
    // Fill past the window (twice over), so the timed loop starts in the steady state: full, with
    // every append matched by a trim, rather than still growing.
    for (; sample < 2 * samplesPerWindow; ++sample)
    {
        const double t = static_cast<double>(sample) * intervalSeconds;
        history.append(t, pointAt(t), false);
        history.trimToWindow(windowSeconds);
    }

    for (auto _ : state)
    {
        const double t = static_cast<double>(sample) * intervalSeconds;
        history.append(t, pointAt(t), false);
        history.trimToWindow(windowSeconds);
        benchmark::DoNotOptimize(history);
        ++sample;
    }
    state.counters["points"] = static_cast<double>(history.size());
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ProcessDetailsHistory_AppendTrimFullWindow)
    ->Args({Domain::Sampling::HISTORY_SECONDS_DEFAULT, Domain::Sampling::REFRESH_INTERVAL_MIN_MS})
    ->Args({Domain::Sampling::HISTORY_SECONDS_MAX, Domain::Sampling::REFRESH_INTERVAL_MIN_MS})
    ->Unit(benchmark::kNanosecond);

} // namespace
