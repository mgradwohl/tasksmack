// Headless cost of the Processes table's inline meters (#1528).
//
// Each iteration builds and renders one whole ImGui frame (NewFrame() through Render()) of a table
// shaped like the Processes table at 1600x1000: 1000 processes behind an ImGuiListClipper, the nine
// meter columns (CPU %, Mem %, Memory, I/O, network, GPU) with right-aligned text in each. The
// MetersOff/MetersOn pair isolates what the meters add per frame: one AddRectFilled per visible
// meter cell, no allocation. The maxima benchmark is the once-per-generation scan the absolute
// columns are scaled against.

#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessRowMeterView.h"
#include "App/ProcessColumnConfig.h"
#include "ChartGeometryScenes.h" // HeadlessChartContext
#include "Domain/ProcessSnapshot.h"

#include <benchmark/benchmark.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

constexpr int PROCESS_COUNT = 1000;

std::vector<Domain::ProcessSnapshot> makeSnapshots()
{
    std::vector<Domain::ProcessSnapshot> snapshots(PROCESS_COUNT);
    for (std::size_t i = 0; i < snapshots.size(); ++i)
    {
        auto& proc = snapshots[i];
        const auto n = static_cast<double>(i % 100);
        proc.pid = static_cast<std::int32_t>(i + 1);
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
                     const App::ProcessRowMeter::ColumnMaxima& maxima)
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

void runTable(benchmark::State& state, bool metersOn)
{
    const ChartGeometry::HeadlessChartContext context;
    const auto snapshots = makeSnapshots();
    App::ProcessRowMeter::Settings settings;
    for (const App::ProcessColumn col : App::ProcessRowMeter::METER_COLUMNS)
    {
        settings.set(col, metersOn);
    }
    App::ProcessRowMeter::ColumnMaxima maxima;
    maxima.rebuild(snapshots);
    int vertices = renderTableFrame(snapshots, settings, maxima); // Let the clipper measure the rows
    for (auto _ : state)
    {
        vertices = renderTableFrame(snapshots, settings, maxima);
        benchmark::DoNotOptimize(vertices);
    }
    state.counters["vertices"] = benchmark::Counter(static_cast<double>(vertices));
}

void BM_ProcessRowMeter_TableFrame_MetersOff(benchmark::State& state)
{
    runTable(state, false);
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_MetersOff)->Unit(benchmark::kMicrosecond);

void BM_ProcessRowMeter_TableFrame_MetersOn(benchmark::State& state)
{
    runTable(state, true);
}
BENCHMARK(BM_ProcessRowMeter_TableFrame_MetersOn)->Unit(benchmark::kMicrosecond);

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

} // namespace
