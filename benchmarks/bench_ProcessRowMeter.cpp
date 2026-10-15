// Headless cost of the Processes table's inline meters (#1528).
//
// Each iteration builds and renders one whole ImGui frame (NewFrame() through Render()) of a table
// shaped like the Processes table at 1600x1000: 1000 processes behind an ImGuiListClipper, the nine
// meter columns (CPU %, Mem %, Memory, I/O, network, GPU) with right-aligned text in each. The
// MetersOff/MetersOn pair isolates what the meters add per frame: one AddRectFilled per visible
// meter cell, no allocation. The maxima benchmark is the once-per-generation scan the absolute
// columns are scaled against.
//
// Sparklines (#1612): the SparklineCpu frame has CPU % as a sparkline and no other meter, the
// SparklinesAll frame every meter column, each with a full 30-point history per row (one
// AddPolyline per visible cell). RowHistoryAdvance is the once-per-generation ring maintenance on the
// sampler thread, for 500 and 1000 processes; its "bytes" counter is what the tracker holds.

#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/ProcessColumnConfig.h"
#include "ChartGeometryScenes.h" // HeadlessChartContext
#include "Domain/ProcessRowHistory.h"
#include "Domain/ProcessSnapshot.h"

#include <benchmark/benchmark.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{

constexpr int PROCESS_COUNT = 1000;

/// @p count processes; @p generation shifts the values, so successive generations differ.
std::vector<Domain::ProcessSnapshot> makeSnapshots(std::size_t count = PROCESS_COUNT, std::size_t generation = 0)
{
    std::vector<Domain::ProcessSnapshot> snapshots(count);
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        auto& proc = snapshots[i];
        const auto n = static_cast<double>((i + (generation * 7)) % 100);
        proc.pid = static_cast<std::int32_t>(i + 1);
        proc.uniqueKey = i + 1;
        proc.cpuPercent = n;
        proc.memoryPercent = n * 0.5;
        proc.memoryBytes = static_cast<std::uint64_t>(i) * 1'048'576U;
        proc.ioReadBytesPerSec = n * 1000.0;
        proc.ioWriteBytesPerSec = n * 500.0;
        proc.netSentBytesPerSec = n * 200.0;
        proc.netReceivedBytesPerSec = n * 300.0;
        proc.gpuUtilPercent = n * 0.3;
        proc.gpuMemoryBytes = static_cast<std::uint64_t>(i) * 4096U;
    }
    return snapshots;
}

/// One frame of the table; returns the frame's vertex count.
int renderTableFrame(const std::vector<Domain::ProcessSnapshot>& snapshots,
                     const App::ProcessRowMeter::Settings& settings,
                     const App::ProcessRowMeter::ColumnMaxima& maxima,
                     const Domain::ProcessRowHistory::Generation* history)
{
    constexpr App::ProcessRowMeter::Colors COLORS{.low = ImVec4(1.0F, 0.8F, 0.2F, 0.15F), .high = ImVec4(1.0F, 0.2F, 0.2F, 0.30F)};
    const bool metersShown = settings.anyOn();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
    ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    constexpr auto COLUMNS = App::ProcessRowMeter::METER_COLUMNS;
    if (ImGui::BeginTable("##processes", static_cast<int>(COLUMNS.size()), ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg))
    {
        for (const App::ProcessColumn col : COLUMNS)
        {
            // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - constexpr literals are null-terminated
            ImGui::TableSetupColumn(App::getColumnInfo(col).name.data(), ImGuiTableColumnFlags_WidthFixed, 120.0F);
        }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(snapshots.size()));
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                const Domain::ProcessSnapshot& proc = snapshots[static_cast<std::size_t>(i)];
                ImGui::TableNextRow();
                int idx = 0;
                for (const App::ProcessColumn col : COLUMNS)
                {
                    if (!ImGui::TableSetColumnIndex(idx++))
                    {
                        continue;
                    }
                    if (metersShown)
                    {
                        App::ProcessRowMeter::renderCellMeter(settings, col, proc, maxima, COLORS);
                        App::ProcessRowMeter::renderCellSparkline(settings, col, history, static_cast<std::size_t>(i), COLORS);
                    }
                    ImGui::TextUnformatted("123.4 MiB");
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
    ImGui::Render();
    return ImGui::GetDrawData()->TotalVtxCount;
}

void runTable(benchmark::State& state, const App::ProcessRowMeter::Settings& settings)
{
    const ChartGeometry::HeadlessChartContext context;
    // A full ring for every process, for the sparkline columns (none when there are none).
    Domain::ProcessRowHistory::Tracker tracker;
    std::shared_ptr<const Domain::ProcessRowHistory::Generation> history;
    std::size_t generation = 0;
    while (generation < Domain::ProcessRowHistory::RING_CAPACITY)
    {
        history = tracker.advance(makeSnapshots(PROCESS_COUNT, generation), settings.sparklineMetrics());
        ++generation;
    }
    const auto snapshots = makeSnapshots(PROCESS_COUNT, generation);
    App::ProcessRowMeter::ColumnMaxima maxima;
    maxima.rebuild(snapshots);
    int vertices = renderTableFrame(snapshots, settings, maxima, history.get()); // Let the clipper measure the rows
    for (auto _ : state)
    {
        vertices = renderTableFrame(snapshots, settings, maxima, history.get());
        benchmark::DoNotOptimize(vertices);
    }
    state.counters["vertices"] = benchmark::Counter(static_cast<double>(vertices));
}

App::ProcessRowMeter::Settings allColumns(App::ProcessRowMeter::Style style)
{
    App::ProcessRowMeter::Settings settings;
    for (const App::ProcessColumn col : App::ProcessRowMeter::METER_COLUMNS)
    {
        settings.setStyle(col, style);
    }
    return settings;
}

void BM_ProcessRowMeter_TableFrame_MetersOff(benchmark::State& state)
{
    runTable(state, allColumns(App::ProcessRowMeter::Style::Off));
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_MetersOff)->Unit(benchmark::kMicrosecond);

void BM_ProcessRowMeter_TableFrame_MetersOn(benchmark::State& state)
{
    runTable(state, allColumns(App::ProcessRowMeter::Style::Bar));
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_MetersOn)->Unit(benchmark::kMicrosecond);

void BM_ProcessRowMeter_TableFrame_SparklineCpu(benchmark::State& state)
{
    App::ProcessRowMeter::Settings settings = allColumns(App::ProcessRowMeter::Style::Off);
    settings.setStyle(App::ProcessColumn::CpuPercent, App::ProcessRowMeter::Style::Sparkline);
    runTable(state, settings);
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_SparklineCpu)->Unit(benchmark::kMicrosecond);

void BM_ProcessRowMeter_TableFrame_SparklinesAll(benchmark::State& state)
{
    runTable(state, allColumns(App::ProcessRowMeter::Style::Sparkline));
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_SparklinesAll)->Unit(benchmark::kMicrosecond);

// The sparklines' ring maintenance, once per published generation (~1 Hz) on the sampler thread:
// state.range(0) processes, state.range(1) metrics (1: CPU % only; 9: every meter column). Each
// iteration carries every ring to a new generation and appends a value.
void BM_ProcessRowMeter_RowHistoryAdvance(benchmark::State& state)
{
    const auto count = static_cast<std::size_t>(state.range(0));
    const Domain::ProcessRowHistory::MetricSet metrics =
        state.range(1) == 1 ? Domain::ProcessRowHistory::bitOf(Domain::ProcessRowHistory::Metric::CpuPercent)
                            : static_cast<Domain::ProcessRowHistory::MetricSet>((1U << Domain::ProcessRowHistory::METRIC_COUNT) - 1U);
    const auto first = makeSnapshots(count, 0);
    const auto second = makeSnapshots(count, 1);
    Domain::ProcessRowHistory::Tracker tracker;
    std::size_t generation = 0;
    while (generation < Domain::ProcessRowHistory::RING_CAPACITY)
    {
        (void) tracker.advance((generation % 2U) == 0U ? first : second, metrics);
        ++generation;
    }
    for (auto _ : state)
    {
        auto history = tracker.advance((generation % 2U) == 0U ? first : second, metrics);
        benchmark::DoNotOptimize(history.get());
        ++generation;
    }
    state.counters["bytes"] = benchmark::Counter(static_cast<double>(tracker.bytes()));
}
BENCHMARK(BM_ProcessRowMeter_RowHistoryAdvance)
    ->Args({500, 1})
    ->Args({500, 9})
    ->Args({1000, 1})
    ->Args({1000, 9})
    ->Unit(benchmark::kMicrosecond);

// The absolute columns' maxima, rebuilt once per adopted generation (~1 Hz), not per frame.
void BM_ProcessRowMeter_MaximaRebuild(benchmark::State& state)
{
    const auto snapshots = makeSnapshots();
    App::ProcessRowMeter::ColumnMaxima maxima;
    for (auto _ : state)
    {
        maxima.rebuild(snapshots);
        benchmark::DoNotOptimize(maxima.max.data());
    }
}
BENCHMARK(BM_ProcessRowMeter_MaximaRebuild)->Unit(benchmark::kMicrosecond);

// The per-frame row format cache lookups (#2115): every visible row asks for its formatted cells
// once per frame, and outside a new generation each is a hit. One iteration is one 50-row frame
// over the 1000-process table, a different window of rows each time.
void BM_ProcessRowMeter_RowFormatCacheHit(benchmark::State& state)
{
    constexpr std::size_t VISIBLE_ROWS = 50;
    constexpr std::uint64_t GENERATION = 1;
    constexpr std::uintptr_t FONT_ID = 1;
    const auto snapshots = makeSnapshots();
    std::unordered_map<std::uint64_t, App::ProcessRowFormat::RowFormatCache> cache;
    for (const auto& proc : snapshots)
    {
        static_cast<void>(App::ProcessRowFormat::getOrBuildRowFormatCache(cache, proc, GENERATION, FONT_ID));
    }
    std::size_t first = 0;
    for (auto _ : state)
    {
        for (std::size_t row = 0; row < VISIBLE_ROWS; ++row)
        {
            const auto& entry =
                App::ProcessRowFormat::getOrBuildRowFormatCache(cache, snapshots[(first + row) % snapshots.size()], GENERATION, FONT_ID);
            benchmark::DoNotOptimize(&entry);
        }
        first = (first + VISIBLE_ROWS) % snapshots.size();
    }
}
BENCHMARK(BM_ProcessRowMeter_RowFormatCacheHit)->Unit(benchmark::kNanosecond);

} // namespace
