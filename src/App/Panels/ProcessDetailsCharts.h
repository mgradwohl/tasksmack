#pragma once

// Process Details' chart tabs (#1179, slice 5): the Overview's CPU, Memory, Power and Resources
// charts, the Network and I/O tab's two charts, and the GPU tab's usage table, per-GPU breakdown and
// two charts, with their empty states.
//
// The view owns only its render state: the CPU chart's reduced points and scratch buffers, the
// Network and GPU tabs' fill state, the GPU tab's "No GPU usage" text and its debug-log throttle. The
// history, the smoothed now-bar values and the snapshot stay the panel's, which passes them in each
// frame (ProcessChartContext); nothing here keeps a pointer into the panel past the call.
//
// The pure pieces (NowBar columns, value text, axis targets, empty-state decisions) are in
// ProcessDetailsChartHelpers.h, tested without ImGui (test_ProcessDetailsChartHelpers.cpp); the
// render itself is run headless in test_ProcessDetailsChartsRender.cpp.

#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_HistoryHelpers.h"
#include "ProcessSmoothedUsage.h"
#include "UI/ChartWidgets.h"
#include "UI/FillPlotLayout.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App
{

/// What the charts draw for one frame, from the panel. Every pointer must be set; a render call with
/// one missing draws nothing. Built for the call and never kept.
struct ProcessChartContext
{
    const Detail::ProcessDetailsHistory* history = nullptr;
    const Detail::ProcessSmoothedUsage* smoothed = nullptr;
    const Domain::ProcessSnapshot* snapshot = nullptr; ///< The displayed sample
    /// Taken whenever the history changes (UI::Widgets::nextChartDataGeneration()), so each chart keeps
    /// its reduced points until then (HistoryChartConfig::dataGeneration, #1139).
    std::uint64_t historyGeneration = 0;
    double maxHistorySeconds = Domain::Numeric::toDouble(Domain::Sampling::HISTORY_SECONDS_DEFAULT);
    double peakMemoryBytes = 0.0; ///< Peak working set (never decreases)
    /// Which of the shown sample's rates and GPU figures are readings (Detail::rateReadings(), #1210).
    Detail::SampleRateReadings rateReadings;
    bool hasSharedMemory = false; ///< ProcessCapabilities::hasSharedMemory: draw the Shared line (#1035)
    bool hasPowerUsage = false;   ///< ProcessCapabilities::hasPowerUsage: draw the Power chart (#1028)
};

/// The chart tabs' content for the process Process Details shows.
class ProcessDetailsCharts
{
  public:
    /// The Overview's charts under its Identity/Runtime block: CPU, Memory, then Power (where the
    /// platform measures it) and Resources, sharing @p fill, the Overview's height (#959).
    void renderOverviewCharts(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);

    /// The GPU tab's body: its empty state, or the usage table, the per-GPU breakdown and the two
    /// history charts.
    void renderGpuTab(const ProcessChartContext& ctx);

    /// The Network and I/O tab's body: its empty state, or the I/O and Network charts.
    void renderNetworkTab(const ProcessChartContext& ctx);

  private:
    void renderCpuUsageSection(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    void drawCpuBandsAndLines(std::span<const double> timeData,
                              std::span<const double> total,
                              std::span<const double> user,
                              std::span<const double> system,
                              std::uint64_t historyGeneration);
    static void renderMemoryUsageSection(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    static void renderThreadAndFaultHistory(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    static void renderPowerUsage(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    static void renderIoStats(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    static void renderNetworkStats(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    void renderGpuUsage(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);
    static void renderGpuCurrentMetricsTable(const ProcessChartContext& ctx);
    static void renderPerGpuBreakdown(const ProcessChartContext& ctx);
    static void renderGpuHistoryGraphs(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill);

    // Render scratch buffers for the stacked CPU chart (reused across frames to avoid per-frame heap
    // allocation): only the reduced points, at most LINE_PLOT_MAX_POINTS_DENSE, are built into them
    // each frame. The User and System bands, their x axis held to now (#1016), shared with the
    // Overview (#1180); the User line is the User band's top.
    UI::Widgets::UserSystemStack m_CpuStack;
    std::vector<double> m_CpuPlotTotal;
    std::vector<double> m_CpuPlotSystem;
    UI::Widgets::ReducedPointsCache m_CpuPlotReduction; // The CPU chart's reduced points (#1022), kept per history generation (#1139)

    // Per-tab state for the shared chart-height rule (#959). The Overview's is the panel's: its
    // Identity/Runtime block shares that tab's height.
    UI::Widgets::PlotFillState m_NetworkFill;
    UI::Widgets::PlotFillState m_GpuFill;

    // The GPU tab's "No GPU usage" explanation, naming the history window (#1210): rebuilt only when
    // the window changes. Keyed by Detail::historyWindowCacheKey() (whole milliseconds, #1487); empty
    // until first built.
    std::string m_NoGpuUsageDetail;
    std::optional<std::int64_t> m_NoGpuUsageDetailKey;

    // GPU logging throttle state (per-panel tracking)
    std::int32_t m_LastGpuLogPid = -1;
    std::uint64_t m_LastGpuLogMemoryBytes = std::numeric_limits<std::uint64_t>::max();
};

} // namespace App
