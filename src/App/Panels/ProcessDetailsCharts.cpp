#include "ProcessDetailsCharts.h"

#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsChartHelpers.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessSmoothedUsage.h"
#include "UI/Card.h"
#include "UI/ChartWidgets.h"
#include "UI/ChromeLayout.h"
#include "UI/ChromeWidgets.h"
#include "UI/EmptyState.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/InlineText.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#ifdef _WIN32
#include "ProcessDetailsPanel_ResourceHelpers.h" // the GDI Objects series (Windows only) is its only user
#endif

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <algorithm> // the GDI Objects series (Windows only) is its only user
#endif

namespace
{

using App::Detail::ProcessSeries;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::formatAxisLocalized;
using UI::Widgets::formatAxisWatts;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::NowBarList;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::plotSeries;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;

// The newest @p count samples of a history, viewed in place (#1018: this was a per-frame copy).
[[nodiscard]] auto tailSpan(std::span<const double> data, std::size_t count) -> std::span<const double>
{
    return UI::Widgets::tailAlignedSpan(data, count).values;
}

// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008).
constexpr const char* CPU_TOTAL_LABEL = "Total";
constexpr const char* CPU_USER_LABEL = "User";
constexpr const char* CPU_SYSTEM_LABEL = "System";
// The resident and peak pair carry the Processes table's column names, so the chart and the table
// call one quantity by one name (#1273).
constexpr const char* MEM_USED_LABEL = "Memory";
constexpr const char* MEM_SHARED_LABEL = "Shared";
#ifdef _WIN32
// Windows' Shared is the shareable working set (#2583), not the pages shared now: its strip entry says so.
constexpr const char* MEM_SHARED_TOOLTIP = "Shareable working set: the pages of its working set that can be shared with other "
                                           "processes (DLLs, mapped files), whether or not another process maps them now. "
                                           "The working set less Memory, the private working set";
#else
constexpr const char* MEM_SHARED_TOOLTIP = nullptr;
#endif
// A series on a chart's right-hand axis ends in " →", pointing at it (setupSecondaryRateAxis(), #1206); in
// its value-strip entry and tooltip rows the mark follows the value (SECONDARY_AXIS_MARK, #1300).
constexpr const char* MEM_VIRTUAL_LABEL = "Virtual →";
// A peak is named "Peak " plus the series it tracks: this chart's series is "Memory", as the
// System chart's "Peak Used" tracks "Used" (#1342). The Processes table keeps its short "Peak Mem"
// header for width, as it does "Mem %".
constexpr const char* MEM_PEAK_LABEL = "Peak Memory";
constexpr const char* MEM_PERCENT_LABEL = "Mem %";                // the Processes table's column (#1878)
constexpr const char* MEM_PRIVATE_LABEL = "Private";              // the Processes table's column (#1885)
constexpr const char* MEM_SWAP_LABEL = "Swap";                    // the Processes table's column (#1885)
constexpr const char* MEM_PSS_LABEL = "PSS";                      // Proportional set size, smaps_rollup (#1885)
constexpr const char* MEM_USS_LABEL = "USS";                      // Unique set size, smaps_rollup (#1885)
constexpr const char* FAULTS_TOTAL_LABEL = "Page Faults (total)"; // the Processes table's Page Faults column (#1878)
constexpr const char* THREADS_LABEL = "Threads";
constexpr const char* FAULTS_LABEL = "Page Faults →"; // Its values carry the "/s" ("12.0/s"), #1202
#ifdef _WIN32
constexpr const char* GDI_LABEL = "GDI Objects";
// On the outer right-hand axis, its own scale (#1916): see ProcessDetailsChartHelpers.h's
// handleAxisDataMax().
constexpr const char* HANDLE_LABEL = "Handles →";
#else
constexpr const char* HANDLE_LABEL = "FDs →";
#endif
constexpr const char* IO_READ_LABEL = "Read";
constexpr const char* IO_WRITE_LABEL = "Write";
constexpr const char* NET_SENT_LABEL = "Sent";
constexpr const char* NET_RECV_LABEL = "Received";
constexpr const char* POWER_LABEL = "Power";
// The GPU and NPU tabs' utilization series
constexpr const char* UTIL_LABEL = "Utilization";
constexpr const char* GPU_MEMORY_LABEL = "Memory";
// The NPU tab's memory series (#1869)
constexpr const char* NPU_DEDICATED_LABEL = "Dedicated";
constexpr const char* NPU_SHARED_LABEL = "Shared";

/// The time axis every chart of one frame shares: the newest history.size() samples, the window's
/// x range, and each sample's x relative to the frame's now.
struct ChartTimeAxis
{
    std::size_t alignedCount = 0;
    double nowSeconds = 0.0;
    UI::Widgets::TimeAxisConfig axisConfig;
    std::span<const double> timeData;
};

[[nodiscard]] ChartTimeAxis chartTimeAxis(const App::Detail::ProcessDetailsHistory& history, double maxHistorySeconds)
{
    ChartTimeAxis axis;
    axis.alignedCount = history.size();
    axis.nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)
    const auto timestamps = tailSpan(history.timestamps(), axis.alignedCount);
    axis.axisConfig = makeTimeAxisConfig(maxHistorySeconds);
    axis.timeData = frameTimeAxis(timestamps, axis.alignedCount, axis.nowSeconds);
    return axis;
}

/// The hovered sample's index on @p timeData when the plot being drawn is hovered and it is below
/// @p count, for its tooltip.
[[nodiscard]] std::optional<std::size_t> hoveredSampleIndex(std::span<const double> timeData, std::size_t count)
{
    if (!ImPlot::IsPlotHovered())
    {
        return std::nullopt;
    }
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x); idxVal && *idxVal < count)
    {
        return idxVal;
    }
    return std::nullopt;
}

/// One series of a two-series rate chart (I/O, Network).
struct RateSeries
{
    const char* label = nullptr;
    std::span<const double> data;
    ImVec4 color;
    ImVec4 fill;
};

/// How a two-series chart's axis and tooltip values read: bytes/sec for I/O and Network, bytes for
/// the NPU tab's memory (#1869).
struct TwoSeriesUnits
{
    ImPlotFormatter axisFormatter = nullptr;
    std::string (*valueText)(double) = nullptr; ///< A sample's tooltip text, N/A for a gap
};

[[nodiscard]] std::string bytesOrNA(double bytes)
{
    return UI::Widgets::formatSampleOrNA(bytes, [](double b) { return UI::Format::formatBytes(b); });
}

constexpr TwoSeriesUnits BYTES_PER_SEC_UNITS{.axisFormatter = formatAxisBytesPerSec, .valueText = UI::Format::formatBytesPerSecOrNA};
constexpr TwoSeriesUnits BYTES_UNITS{.axisFormatter = UI::Widgets::formatAxisBytes, .valueText = bytesOrNA};

/// Draws a two-series chart -- @p primary at the primary series' weight, @p secondary as the first
/// secondary -- on an axis in @p units from 0 to @p upper, with its hover tooltip. @p notice, when set,
/// is shown over the plot in place of the "collecting" hint: the chart has no readings (#1918).
void drawTwoSeriesPlot(const char* plotId,
                       const ChartTimeAxis& axis,
                       double upper,
                       float plotHeight,
                       std::uint64_t historyGeneration,
                       const TwoSeriesUnits& units,
                       const RateSeries& primary,
                       const RateSeries& secondary,
                       const char* notice = nullptr)
{
    const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
        UI::Widgets::withHeight(
            UI::Widgets::rateHistoryConfigWithUpper(plotId, axis.axisConfig.xMin, axis.axisConfig.xMax, units.axisFormatter, upper),
            plotHeight),
        historyGeneration));
    if (!chart.active())
    {
        return;
    }
    if (notice != nullptr)
    {
        UI::Widgets::drawPlotNotice(notice);
    }
    else
    {
        UI::Widgets::drawCollectingHint(axis.alignedCount);
    }
    const int plotCount = UI::Format::checkedCount(axis.alignedCount);
    plotSeries(
        primary.label, axis.timeData.data(), primary.data.data(), plotCount, primary.color, primary.fill, seriesStyle(SeriesRole::Primary));
    plotSeries(secondary.label,
               axis.timeData.data(),
               secondary.data.data(),
               plotCount,
               secondary.color,
               secondary.fill,
               seriesStyle(SeriesRole::Secondary, 0));

    if (const auto idx = hoveredSampleIndex(axis.timeData, axis.alignedCount))
    {
        const std::array rows{
            UI::Widgets::TooltipRow{.label = primary.label, .color = primary.color, .value = units.valueText(primary.data[*idx])},
            UI::Widgets::TooltipRow{.label = secondary.label, .color = secondary.color, .value = units.valueText(secondary.data[*idx])},
        };
        UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
    }
}

/// The Memory chart's series and what its tooltip needs.
struct MemoryChartData
{
    std::span<const double> used;
    std::span<const double> shared; ///< Empty where Shared is not drawn (#1035)
    std::span<const double> virtualBytes;
    std::span<const double> privateBytes; ///< Empty where the platform doesn't read Private (#1885)
    std::span<const double> swapBytes;    ///< Empty where the platform doesn't read Swap (#1885)
    double peakBytes = 0.0;
    double percentPerByte = 0.0;
};

void memoryTooltip(const ChartTimeAxis& axis, const MemoryChartData& data)
{
    const auto& scheme = UI::Theme::get().scheme();
    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
    const auto idxVal = hoveredIndexFromPlotX(axis.timeData, mouse.x);
    if (!idxVal)
    {
        return;
    }
    std::vector<UI::Widgets::TooltipRow> rows;
    if (*idxVal < data.used.size())
    {
        rows.push_back({.label = MEM_USED_LABEL,
                        .color = scheme.chartMemory,
                        .value = App::Detail::formatBytesWithRamShare(data.used[*idxVal], data.percentPerByte)});
    }
    if (*idxVal < data.shared.size())
    {
        rows.push_back({.label = MEM_SHARED_LABEL,
                        .color = scheme.chartMemoryShared,
                        .value = App::Detail::formatBytesWithRamShare(data.shared[*idxVal], data.percentPerByte)});
    }
    if (*idxVal < data.virtualBytes.size())
    {
        rows.push_back(
            {.label = MEM_VIRTUAL_LABEL,
             .color = scheme.chartMemoryVirtual,
             .value = UI::Widgets::formatSampleOrNA(data.virtualBytes[*idxVal], [](double v) { return UI::Format::formatBytes(v); })});
    }
    if (*idxVal < data.privateBytes.size())
    {
        rows.push_back({.label = MEM_PRIVATE_LABEL,
                        .color = scheme.chartMemoryPrivate,
                        .value = App::Detail::formatBytesWithRamShare(data.privateBytes[*idxVal], data.percentPerByte)});
    }
    if (*idxVal < data.swapBytes.size())
    {
        rows.push_back(
            {.label = MEM_SWAP_LABEL, .color = scheme.chartMemorySwap, .value = UI::Format::formatBytes(data.swapBytes[*idxVal])});
    }
    if (data.peakBytes > 0.0)
    {
        // The line's colour: this row was textWarning, which matched nothing (#1005).
        rows.push_back({.label = MEM_PEAK_LABEL, .color = scheme.chartPeakLine, .value = UI::Format::formatBytes(data.peakBytes)});
    }
    UI::Widgets::renderHistoryTooltip(axis.timeData[*idxVal], rows);
}

/// The Memory chart's lines inside its plot: the peak reference line, Used, Shared, Private and Swap
/// on the left axis, Virtual on the right, and the tooltip.
void drawMemorySeries(const ChartTimeAxis& axis, const MemoryChartData& data)
{
    const auto& scheme = UI::Theme::get().scheme();
    // Draw peak working set as a horizontal reference line (never decreases)
    if (data.peakBytes > 0.0)
    {
        // Draw horizontal line at peak value across the entire X range
        const double peakY = data.peakBytes;
        std::array<double, 2> peakX = {axis.axisConfig.xMin, axis.axisConfig.xMax};
        std::array<double, 2> peakYVals = {peakY, peakY};
        ImPlot::PlotLine(MEM_PEAK_LABEL,
                         peakX.data(),
                         peakYVals.data(),
                         2,
                         {ImPlotProp_LineColor, scheme.chartPeakLine, ImPlotProp_LineWeight, UI::Widgets::lineWeight(1.5F)});
    }

    if (!data.used.empty())
    {
        plotSeries(MEM_USED_LABEL,
                   axis.timeData.data(),
                   data.used.data(),
                   UI::Format::checkedCount(data.used.size()),
                   scheme.chartMemory,
                   scheme.chartMemoryFill,
                   seriesStyle(SeriesRole::Primary));
    }

    if (!data.shared.empty())
    {
        plotSeries(MEM_SHARED_LABEL,
                   axis.timeData.data(),
                   data.shared.data(),
                   UI::Format::checkedCount(data.shared.size()),
                   scheme.chartMemoryShared,
                   scheme.chartMemorySharedFill,
                   seriesStyle(SeriesRole::Secondary, 0));
    }

    // Private and Swap (#1885): lines in bytes beside Used and Shared, each with its own marker.
    if (!data.privateBytes.empty())
    {
        plotSeries(MEM_PRIVATE_LABEL,
                   axis.timeData.data(),
                   data.privateBytes.data(),
                   UI::Format::checkedCount(data.privateBytes.size()),
                   scheme.chartMemoryPrivate,
                   scheme.chartMemoryPrivateFill,
                   seriesStyle(SeriesRole::Secondary, 2));
    }
    if (!data.swapBytes.empty())
    {
        plotSeries(MEM_SWAP_LABEL,
                   axis.timeData.data(),
                   data.swapBytes.data(),
                   UI::Format::checkedCount(data.swapBytes.size()),
                   scheme.chartMemorySwap,
                   scheme.chartMemorySwapFill,
                   seriesStyle(SeriesRole::Secondary, 3));
    }

    if (!data.virtualBytes.empty())
    {
        // Line only: a fill on its own scale would cover the Used and Shared areas.
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        plotSeries(MEM_VIRTUAL_LABEL,
                   axis.timeData.data(),
                   data.virtualBytes.data(),
                   UI::Format::checkedCount(data.virtualBytes.size()),
                   scheme.chartMemoryVirtual,
                   scheme.chartMemoryVirtualFill,
                   seriesStyle(SeriesRole::Secondary, 1));
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
    }

    if (ImPlot::IsPlotHovered())
    {
        memoryTooltip(axis, data);
    }
}

/// The Memory chart's NowBars: Memory and Shared (where it is drawn, @p showShared) on the bytes axis
/// (@p memAxisUpper), Virtual on its own (@p virtAxisUpper). Smoothed like every other NowBar (#1012);
/// these were the raw latest history sample, so they stepped while the bars around them glided.
[[nodiscard]] NowBarList
memoryNowBars(const App::Detail::ProcessSmoothedUsage& smoothed, bool showShared, double memAxisUpper, double virtAxisUpper)
{
    const auto& theme = UI::Theme::get();
    const double usedNow = smoothed.residentBytes;
    const double sharedNow = smoothed.memorySharedBytes;
    const double percentPerByte = smoothed.memoryPercentPerByte;
    NowBarList memoryBars;
    // Used and Shared carry their share of RAM in tooltipText (shown on hover and in the value strip);
    // Virtual has none, being mostly reserved address space.
    memoryBars.push_back(
        {.valueText = UI::Format::formatBytes(usedNow),
         .label = MEM_USED_LABEL,
         .tooltipText = UI::InlineText::format("{}: {}", MEM_USED_LABEL, App::Detail::formatBytesWithRamShare(usedNow, percentPerByte)),
         .value01 = UI::Widgets::normalizeToUnitInterval(usedNow, memAxisUpper),
         .color = theme.scheme().chartMemory});
    if (showShared)
    {
        memoryBars.push_back({
            .valueText = UI::Format::formatBytes(sharedNow),
            .label = MEM_SHARED_LABEL,
            .tooltipText =
                UI::InlineText::format("{}: {}", MEM_SHARED_LABEL, App::Detail::formatBytesWithRamShare(sharedNow, percentPerByte)),
            .value01 = UI::Widgets::normalizeToUnitInterval(sharedNow, memAxisUpper),
            .color = theme.scheme().chartMemoryShared,
            .stripTooltip = MEM_SHARED_TOOLTIP,
        });
    }
    memoryBars.push_back({.valueText = UI::Format::formatBytes(smoothed.virtualBytes),
                          .label = MEM_VIRTUAL_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.virtualBytes, virtAxisUpper),
                          .color = theme.scheme().chartMemoryVirtual});

    return memoryBars;
}

/// The Resources chart's series: Threads (and GDI Objects on Windows, possibly shorter, drawn from its
/// offset, #1001) on the left axis, Page Faults on the right (#1024), and Handles/FDs on a right-hand
/// axis of their own outside it (#1916).
struct ResourceChartData
{
    std::span<const double> threads;
    std::span<const double> handles;
    std::span<const double> faults;
#ifdef _WIN32
    std::span<const double> gdi;
    std::size_t gdiTimeOffset = 0;
    bool hasGdiSamples = false;
#endif
};

void resourceTooltip(const ChartTimeAxis& axis, const ResourceChartData& data)
{
    const auto& theme = UI::Theme::get();
    const auto idx = hoveredSampleIndex(axis.timeData, axis.alignedCount);
    if (!idx)
    {
        return;
    }
    std::vector<UI::Widgets::TooltipRow> rows{
        {.label = THREADS_LABEL, .color = theme.scheme().chartThreads, .value = App::Detail::formatCountOrNA(data.threads[*idx])},
        {.label = HANDLE_LABEL, .color = theme.scheme().chartHandles, .value = App::Detail::formatCountOrNA(data.handles[*idx])},
        {.label = FAULTS_LABEL,
         .color = theme.scheme().chartPageFaults,
         .value = UI::Widgets::formatSampleOrNA(data.faults[*idx], [](double v) { return UI::Format::formatCountPerSecond(v); })},
    };
#ifdef _WIN32
    if (data.hasGdiSamples)
    {
        const auto gdiValue = App::Detail::seriesValueAt(data.gdi, data.gdiTimeOffset, *idx);
        rows.push_back({.label = GDI_LABEL,
                        .color = theme.scheme().chartGdi,
                        .value = gdiValue ? App::Detail::formatCountOrNA(*gdiValue) : std::string("N/A")});
    }
#endif
    UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
}

/// The Resources chart's lines inside its plot, and the tooltip.
void drawResourceSeries(const ChartTimeAxis& axis, const ResourceChartData& data)
{
    const auto& theme = UI::Theme::get();
    const int plotCount = UI::Format::checkedCount(axis.alignedCount);
    // Each line fills its own axis, so a steady thread count and a steady handle count sit at the same
    // height: Threads, the chart's primary series, is drawn after Handles, on top of it (#1916). A CSV
    // copy keeps the key's column order: a series' first export is the one kept.
    UI::Widgets::exportSeries(THREADS_LABEL, axis.timeData, data.threads);
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y3);
    plotSeries(HANDLE_LABEL,
               axis.timeData.data(),
               data.handles.data(),
               plotCount,
               theme.scheme().chartHandles,
               theme.scheme().chartHandlesFill,
               seriesStyle(SeriesRole::Secondary, 0));
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
    plotSeries(THREADS_LABEL,
               axis.timeData.data(),
               data.threads.data(),
               plotCount,
               theme.scheme().chartThreads,
               theme.scheme().chartThreadsFill,
               seriesStyle(SeriesRole::Primary));
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
    plotSeries(FAULTS_LABEL,
               axis.timeData.data(),
               data.faults.data(),
               plotCount,
               theme.scheme().chartPageFaults,
               std::nullopt,
               seriesStyle(SeriesRole::Secondary, 1));
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);

#ifdef _WIN32
    if (data.hasGdiSamples && data.gdiTimeOffset < axis.timeData.size())
    {
        const int gdiPlotCount = UI::Format::checkedCount(std::min(data.gdi.size(), axis.timeData.size() - data.gdiTimeOffset));
        plotSeries(GDI_LABEL,
                   axis.timeData.subspan(data.gdiTimeOffset).data(),
                   data.gdi.data(),
                   gdiPlotCount,
                   theme.scheme().chartGdi,
                   std::nullopt,
                   seriesStyle(SeriesRole::Secondary, 2));
    }
#endif

    resourceTooltip(axis, data);
}

/// The Resources chart's axes' upper bounds, which its lines and NowBars are both scaled to.
struct ResourceAxisBounds
{
    double threads = 0.0; ///< The left axis: Threads, and GDI Objects on Windows
    double faults = 0.0;  ///< Y2: Page Faults/s (#1024)
    double handles = 0.0; ///< Y3: Handles/FDs (#1916)
};

/// The Resources chart's NowBars, each scaled to its series' axis (#1003): Threads, Handles/FDs, Page
/// Faults, and GDI Objects on Windows -- four there, the PROCESS_OVERVIEW_NOW_BAR_COLUMNS the
/// Overview reserves. A count or rate with no reading is drawn as an N/A bar (NowBar::available), not
/// as a measured 0 (#1935): @p hasPageFaults is ProcessCapabilities::hasPageFaults.
[[nodiscard]] NowBarList
resourceNowBars(const App::Detail::ProcessSmoothedUsage& smoothed, const ResourceAxisBounds& bounds, bool hasPageFaults)
{
    const auto& theme = UI::Theme::get();
    // A thread count that was not read (0, #1935) shows N/A, as its line shows a gap.
    const NowBar threadsBar{
        .valueText = App::Detail::countTextOrNA(smoothed.threadCountAvailable, smoothed.threadCount),
        .label = THREADS_LABEL,
        .tooltipText = {}, // The fallback, "Threads: <value>", says it all (#1019)
        .value01 = smoothed.threadCountAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.threadCount, bounds.threads) : 0.0,
        .color = theme.scheme().chartThreads,
        .marker = ImPlotMarker_None,
        .available = smoothed.threadCountAvailable};
    // An unreadable count (#1110) shows N/A, as its line shows a gap.
    const NowBar handlesBar{
        .valueText = App::Detail::countTextOrNA(smoothed.handleCountAvailable, smoothed.handleCount),
        .label = HANDLE_LABEL,
        .tooltipText = {}, // The fallback, "<label>: <value>", says it all (#1019)
        .value01 = smoothed.handleCountAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.handleCount, bounds.handles) : 0.0,
        .color = theme.scheme().chartHandles,
        .marker = ImPlotMarker_None,
        .available = smoothed.handleCountAvailable};
    const NowBar faultsBar{.valueText = hasPageFaults ? UI::Format::formatCountPerSecond(smoothed.pageFaultsPerSec) : std::string("N/A"),
                           .label = FAULTS_LABEL,
                           .tooltipText = {},
                           .value01 = hasPageFaults ? UI::Widgets::normalizeToUnitInterval(smoothed.pageFaultsPerSec, bounds.faults) : 0.0,
                           // The line's colour: this bar was chartIo while its line is accentColor(3) (#1004).
                           .color = theme.scheme().chartPageFaults,
                           .marker = ImPlotMarker_None,
                           .available = hasPageFaults};
#ifdef _WIN32
    // GDI objects NowBar (Windows-only). A missing reading is NaN: it is skipped by the axis bound and
    // shown as N/A, and a series with no reading at all -- a process TaskSmack cannot open -- is not
    // drawn (#1000).
    const NowBar gdiBar{.valueText = App::Detail::countTextOrNA(smoothed.gdiInitialized, smoothed.gdiObjectCount),
                        .label = GDI_LABEL,
                        .tooltipText = {}, // The fallback, "GDI Objects: <value>", says it all (#1019)
                        .value01 =
                            smoothed.gdiInitialized ? UI::Widgets::normalizeToUnitInterval(smoothed.gdiObjectCount, bounds.threads) : 0.0,
                        .color = theme.scheme().chartGdi,
                        .marker = ImPlotMarker_None,
                        .available = smoothed.gdiInitialized};
#endif

    NowBarList bars;
    bars.push_back(threadsBar);
    bars.push_back(handlesBar);
    bars.push_back(faultsBar);
#ifdef _WIN32
    bars.push_back(gdiBar);
#endif
    return bars;
}

/// One GPU history chart (Utilization or Memory): a line with fill on @p config's axis, its tooltip,
/// or a dummy item while there is no history. @p formatValue gives the tooltip's value.
template<typename FormatValue>
void drawGpuHistoryPlot(const UI::Widgets::HistoryChartConfig& config,
                        const ChartTimeAxis& axis,
                        std::span<const double> data,
                        const char* label,
                        const char* dummyLabel,
                        const ImVec4& color,
                        const ImVec4& fillColor,
                        const FormatValue& formatValue)
{
    const UI::Widgets::HistoryChart chart(config);
    if (!chart.active())
    {
        return;
    }
    UI::Widgets::drawCollectingHint(axis.alignedCount);
    const int plotCount = UI::Format::checkedCount(axis.alignedCount);
    if (plotCount <= 0)
    {
        ImPlot::PlotDummy(dummyLabel);
        return;
    }
    plotLineWithFill(
        label, axis.timeData.data(), data.data(), plotCount, color, fillColor, 2.0F, true, UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

    // Tooltip
    if (const auto idx = hoveredSampleIndex(axis.timeData, axis.alignedCount))
    {
        const std::array rows{UI::Widgets::TooltipRow{.label = label, .color = color, .value = formatValue(data[*idx])}};
        UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
    }
}

/// One label/value row of a usage table (the GPU and NPU tabs'): the value in @p color, or the default
/// text colour without one.
void labelValueRow(const char* label, const std::string& value, const ImVec4* color)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    if (color != nullptr)
    {
        ImGui::TextColored(*color, "%s", value.c_str());
    }
    else
    {
        ImGui::TextUnformatted(value.c_str());
    }
}

/// What the GPU or NPU tab says when it has nothing to chart (#1210, #1869).
struct DeviceTabEmptyText
{
    const char* unavailableHeading = nullptr;
    const char* unavailableDetail = nullptr;
    const char* noReadingsHeading = nullptr;
    const char* noReadingsDetail = nullptr;
    const char* noUsageHeading = nullptr;
    std::string (*noUsageDetail)(double historySeconds) = nullptr; ///< Names the history window
};

constexpr DeviceTabEmptyText GPU_TAB_EMPTY_TEXT{
    // Not "no usage": without per-process metrics none can be seen (#1210).
    .unavailableHeading = ICON_FA_MICROCHIP "  Per-process GPU usage is not available",
    .unavailableDetail = "This system's GPU monitoring does not report GPU usage per process.",
    // Every retained read failed: nothing is known yet about this process's GPU use (#1210).
    .noReadingsHeading = ICON_FA_MICROCHIP "  No GPU readings yet",
    .noReadingsDetail = "Reading this process's GPU usage has not succeeded yet.",
    .noUsageHeading = ICON_FA_MICROCHIP "  No GPU usage",
    .noUsageDetail = App::Detail::noGpuUsageDetail,
};

constexpr DeviceTabEmptyText NPU_TAB_EMPTY_TEXT{
    .unavailableHeading = ICON_FA_BRAIN "  Per-process NPU usage is not available",
    .unavailableDetail = "This system's GPU monitoring does not report NPU usage per process.",
    .noReadingsHeading = ICON_FA_BRAIN "  No NPU readings yet",
    .noReadingsDetail = "Reading this process's NPU usage has not succeeded yet.",
    .noUsageHeading = ICON_FA_BRAIN "  No NPU usage",
    .noUsageDetail = App::Detail::noNpuUsageDetail,
};

/// Draws the GPU or NPU tab's empty state for @p content, and returns whether it did: false for
/// GpuTabContent::Usage, which the tab charts instead. The "no usage" text names the history window
/// (#1210), so it is kept in @p cachedDetail and rebuilt only when the window changes: its whole
/// milliseconds, @p cachedKey, are an exact integer compare, not float equality (#1487).
bool renderDeviceTabEmptyState(App::Detail::GpuTabContent content,
                               const DeviceTabEmptyText& text,
                               double historySeconds,
                               std::string& cachedDetail,
                               std::optional<std::int64_t>& cachedKey)
{
    switch (content)
    {
    case App::Detail::GpuTabContent::Unavailable:
        UI::Widgets::renderEmptyState(text.unavailableHeading, text.unavailableDetail);
        return true;
    case App::Detail::GpuTabContent::NoReadings:
        UI::Widgets::renderEmptyState(text.noReadingsHeading, text.noReadingsDetail);
        return true;
    case App::Detail::GpuTabContent::NoUsage:
    {
        const std::int64_t windowKey = App::Detail::historyWindowCacheKey(historySeconds);
        if (!cachedKey.has_value() || *cachedKey != windowKey)
        {
            cachedDetail = text.noUsageDetail(historySeconds);
            cachedKey = windowKey;
        }
        UI::Widgets::renderEmptyState(text.noUsageHeading, cachedDetail.c_str());
        return true;
    }
    case App::Detail::GpuTabContent::Usage:
        break;
    }
    return false;
}

/// One device tab's utilization history chart (the GPU's or the NPU's): its IDs and heading.
struct UtilizationChartIds
{
    const char* plotId = nullptr;   ///< The plot's
    const char* cardId = nullptr;   ///< Its card's
    const char* layoutId = nullptr; ///< renderHistoryWithNowBars()' layout id
    const char* heading = nullptr;  ///< The card's heading
    std::size_t nowBarColumns = 1;  ///< The tab's NowBar columns (#1206)
};

/// A device tab's utilization history chart in its card, with its NowBar: @p data on a locked 0-100
/// axis, and the bar at @p smoothedPercent, or N/A when it is not @p available (#1210).
void renderUtilizationChartCard(const UtilizationChartIds& ids,
                                const ChartTimeAxis& axis,
                                std::span<const double> data,
                                bool available,
                                double smoothedPercent,
                                std::uint64_t historyGeneration,
                                UI::Widgets::FillPlotLayout& fill)
{
    const auto& scheme = UI::Theme::get().scheme();
    auto plot = [&]()
    {
        drawGpuHistoryPlot(
            UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig(ids.plotId, axis.axisConfig.xMin, axis.axisConfig.xMax),
                                        fill.plotHeight()),
                historyGeneration),
            axis,
            data,
            UTIL_LABEL,
            UTIL_LABEL,
            scheme.gpuUtilization,
            scheme.gpuUtilizationFill,
            [](double v) { return UI::Format::percentOneDecimal(v); });
    };
    const NowBar bar{
        .valueText = App::Detail::gpuUtilizationText(available, smoothedPercent),
        .label = UTIL_LABEL,
        .tooltipText = {},
        .value01 = available ? UI::Format::percent01(smoothedPercent) : 0.0,
        .color = scheme.gpuUtilization,
        .marker = ImPlotMarker_None,
        .available = available, // N/A is drawn hatched, not as a 0 bar (#1935)
    };
    // Each chart in its own card with its heading inside, as on the other Process Details tabs (#1594).
    if (UI::Widgets::beginChartCard(ids.cardId, fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_CHART_LINE, ids.heading, {}, axis.alignedCount);
        renderHistoryWithNowBars(ids.layoutId, fill.plotHeight(), plot, {bar}, {.minBarColumns = ids.nowBarColumns});
    }
    UI::Widgets::endChartCard(ids.cardId, fill.plotHeight());
    fill.addPlot();
}

} // namespace

namespace App
{

void ProcessDetailsCharts::renderOverviewCharts(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    if (ctx.history == nullptr || ctx.smoothed == nullptr || ctx.snapshot == nullptr)
    {
        return;
    }
    renderCpuUsageSection(ctx, fill);
    renderMemoryUsageSection(ctx, fill);
    // Only where the platform measures it: Windows does not, and used to chart a fabricated figure (#1028).
    if (ctx.hasPowerUsage)
    {
        renderPowerUsage(ctx, fill);
    }
    renderThreadAndFaultHistory(ctx, fill);
}

// Renders the inline CPU history chart (total/user/system) plus paired "now" bars.
void ProcessDetailsCharts::renderCpuUsageSection(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    // Inline CPU history with paired now bar
    if (history.empty())
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const std::size_t alignedCount = axis.alignedCount;
    const auto cpuData = tailSpan(history.series(ProcessSeries::CpuTotal), alignedCount);
    const auto cpuUserData = tailSpan(history.series(ProcessSeries::CpuUser), alignedCount);
    const auto cpuSystemData = tailSpan(history.series(ProcessSeries::CpuSystem), alignedCount);

    // The process's CPU is a percent of the whole machine, so a fixed 0-100 axis drew a flat line
    // for any typical process: one busy thread on 16 logical CPUs is 6.25 %. The axis scales to the
    // data instead, from a 5 % floor up to 100, eased like a rate axis, and the bars share its bound
    // so each bar meets its line (#1195, #1003). Values show one decimal, as the table does. Every
    // axis here is sized to the samples in the window, not the one trimming keeps left of it (#1145).
    const double cpuAxisUpper = UI::Widgets::easedPercentAxisUpperBound(
        "##ProcOverviewCPU", Detail::cpuAxisDataMax(axis.timeData, axis.axisConfig.xMin, cpuData, cpuUserData, cpuSystemData, smoothed));

    // Use smoothed values for NowBars for consistent animation
    const NowBar cpuTotalNow{.valueText = UI::Format::percentOneDecimal(smoothed.cpuPercent),
                             .label = CPU_TOTAL_LABEL,
                             .tooltipText = {},
                             .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.cpuPercent, cpuAxisUpper),
                             .color = theme.scheme().chartCpuTotal}; // The Total line's colour (#1192)
    const NowBar cpuUserNow{.valueText = UI::Format::percentOneDecimal(smoothed.cpuUserPercent),
                            .label = CPU_USER_LABEL,
                            .tooltipText = {},
                            .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.cpuUserPercent, cpuAxisUpper),
                            .color = theme.scheme().cpuUser};
    const NowBar cpuSystemNow{.valueText = UI::Format::percentOneDecimal(smoothed.cpuSystemPercent),
                              .label = CPU_SYSTEM_LABEL,
                              .tooltipText = {},
                              .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.cpuSystemPercent, cpuAxisUpper),
                              .color = theme.scheme().cpuSystem};

    auto cpuPlot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(
                UI::Widgets::rateHistoryConfigWithUpper(
                    "##ProcOverviewCPU", axis.axisConfig.xMin, axis.axisConfig.xMax, UI::Widgets::formatAxisPercent, cpuAxisUpper),
                fill.plotHeight()),
            ctx.historyGeneration));
        if (!chart.active())
        {
            return;
        }
        UI::Widgets::drawCollectingHint(alignedCount);
        // alignedCount > 0 here: the section only renders with history (see above).
        drawCpuBandsAndLines(axis.timeData, cpuData, cpuUserData, cpuSystemData, ctx.historyGeneration);

        if (const auto idx = hoveredSampleIndex(axis.timeData, alignedCount))
        {
            // Total in its line's colour, not a usage-threshold colour, which matched nothing on the chart.
            const std::array rows{
                UI::Widgets::TooltipRow{
                    .label = CPU_TOTAL_LABEL, .color = theme.scheme().chartCpuTotal, .value = UI::Format::percentOneDecimal(cpuData[*idx])},
                UI::Widgets::TooltipRow{
                    .label = CPU_USER_LABEL, .color = theme.scheme().cpuUser, .value = UI::Format::percentOneDecimal(cpuUserData[*idx])},
                UI::Widgets::TooltipRow{.label = CPU_SYSTEM_LABEL,
                                        .color = theme.scheme().cpuSystem,
                                        .value = UI::Format::percentOneDecimal(cpuSystemData[*idx])},
            };
            UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
        }
    };

    if (UI::Widgets::beginChartCard("##ProcCpuCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_MICROCHIP, "CPU", {}, alignedCount);
        renderHistoryWithNowBars("ProcessCPUHistoryOverview",
                                 fill.plotHeight(),
                                 cpuPlot,
                                 {cpuTotalNow, cpuUserNow, cpuSystemNow},
                                 {.minBarColumns = Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS});
    }
    UI::Widgets::endChartCard("##ProcCpuCard", fill.plotHeight());
    fill.addPlot();
}

// The CPU chart's User and System bands and its Total, User and System lines, inside its plot.
void ProcessDetailsCharts::drawCpuBandsAndLines(std::span<const double> timeData,
                                                std::span<const double> total,
                                                std::span<const double> user,
                                                std::span<const double> system,
                                                std::uint64_t historyGeneration)
{
    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // The frame's, as chartTimeAxis() took it

    // What is drawn below is reduced, and the System line is a cumulative top: "Copy data as CSV"
    // takes each series whole and as its own values instead (#808).
    UI::Widgets::exportSeries(CPU_TOTAL_LABEL, timeData, total);
    UI::Widgets::exportSeries(CPU_USER_LABEL, timeData, user);
    UI::Widgets::exportSeries(CPU_SYSTEM_LABEL, timeData, system);

    // These bands and lines are drawn with ImPlot directly, so they are capped here like every
    // plotLineWithFill series (#1022), and to the same budget: what this plot's width calls for
    // (currentPlotPointBudget(), #1411, #843), not a fixed count, so the chart in a narrow details pane
    // draws a fraction of a wide one's points while each bucket still keeps every series' extremes.
    // Reduced together, so the bands still line up with each other and with the lines. Points are
    // chosen by each drawn value -- User (also the user band's top), System and Total -- not by the
    // cumulative system top, which stays flat when System rises as User falls and would drop that spike.
    //
    // The choice of points is kept until the history changes (historyGeneration, #1139), or the budget
    // does (a resize; it is part of the key): reducing and copying the whole history every frame was
    // most of this chart's cost at the largest history settings. Each frame only builds the kept
    // points, at most LINE_PLOT_MAX_POINTS_DENSE of them, into reused member buffers.
    const int pointBudget = UI::Widgets::currentPlotPointBudget();
    const std::span<const UI::Widgets::ReducedPoint> points = m_CpuPlotReduction.points(
        {.generation = historyGeneration, .dataId = 0, .count = timeData.size(), .maxOut = pointBudget},
        [&](std::vector<UI::Widgets::ReducedPoint>& out)
        { UI::Widgets::reduceAlignedPoints<double>(timeData, {user, system, total}, pointBudget, nowSeconds, out); });

    // The User and System bands' edges (shared with the Overview, #1180), plus the Total and System
    // lines. A gap point is NaN in every series (see UI::Widgets::reduceAlignedSeries). The User line
    // is the User band's top, so it is drawn from that.
    auto& stack = m_CpuStack;
    UI::Widgets::buildUserSystemStack<double>(points, timeData, user, system, stack);
    UI::Widgets::gatherReducedValues<double>(points, total, m_CpuPlotTotal);
    UI::Widgets::gatherReducedValues<double>(points, system, m_CpuPlotSystem);

    // Bands and lines reach "now" like every plotLineWithFill series: the last sample held to x = 0
    // (#1016), unless it is too old to pass for current (#1147). Built in their own buffers, so the
    // tooltip's lookup over timeData still finds real samples only.
    UI::Widgets::holdLastValuesToNow(stack.x,
                                     {&stack.base, &stack.userTop, &stack.systemTop, &m_CpuPlotTotal, &m_CpuPlotSystem},
                                     UI::Widgets::maxHoldSecondsForAxis(timeData));
    const int drawCount = UI::Format::checkedCount(stack.x.size());

    // The bands share their labels with the User and System lines below, so ImPlot treats each band
    // and its line as one item. Each band is filled only where both of its own edges have a reading,
    // so a reading missing from one band alone can't feed NaN to the other (#1149).
    UI::Widgets::plotShadedBand(
        CPU_USER_LABEL, stack.x.data(), stack.base.data(), stack.userTop.data(), drawCount, theme.scheme().cpuUserFill);
    UI::Widgets::plotShadedBand(
        CPU_SYSTEM_LABEL, stack.x.data(), stack.userTop.data(), stack.systemTop.data(), drawCount, theme.scheme().cpuSystemFill);

    // Total at the primary series' weight; it has no fill of its own, the bands above are the fill.
    // User and System are secondaries: lighter lines, each with its own marker shape (shown on its
    // value-strip swatch too), so they differ by more than colour (#1198).
    ImPlot::PlotLine(CPU_TOTAL_LABEL,
                     stack.x.data(),
                     m_CpuPlotTotal.data(),
                     drawCount,
                     {ImPlotProp_LineColor,
                      theme.scheme().chartCpuTotal,
                      ImPlotProp_LineWeight,
                      UI::Widgets::lineWeight(UI::Widgets::PRIMARY_SERIES_WEIGHT)});
    UI::Widgets::plotStyledLine(
        CPU_USER_LABEL, stack.x.data(), stack.userTop.data(), drawCount, theme.scheme().cpuUser, seriesStyle(SeriesRole::Secondary, 0));
    UI::Widgets::plotStyledLine(CPU_SYSTEM_LABEL,
                                stack.x.data(),
                                m_CpuPlotSystem.data(),
                                drawCount,
                                theme.scheme().cpuSystem,
                                seriesStyle(SeriesRole::Secondary, 1));
}

// Renders the inline memory history chart (used/shared/virtual, with a peak-line overlay) plus paired
// "now" bars.
void ProcessDetailsCharts::renderMemoryUsageSection(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    // Inline history for memory (overview) mirroring system memory chart layout
    if (history.empty())
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const std::size_t alignedCount = axis.alignedCount;

    // Shared's line, tooltip row and bar are left out where the platform doesn't report it, rather than
    // shown as a permanent 0 (#1035). Windows reports it as the shareable working set (#2583).
    const bool showShared = ctx.hasSharedMemory;
    const MemoryChartData data{
        .used = tailSpan(history.series(ProcessSeries::MemoryUsed), alignedCount),
        .shared = showShared ? tailSpan(history.series(ProcessSeries::MemoryShared), alignedCount) : std::span<const double>{},
        .virtualBytes = tailSpan(history.series(ProcessSeries::Virtual), alignedCount),
        .privateBytes =
            ctx.hasPrivateBytes ? tailSpan(history.series(ProcessSeries::MemoryPrivate), alignedCount) : std::span<const double>{},
        .swapBytes = ctx.hasSwapBytes ? tailSpan(history.series(ProcessSeries::MemorySwap), alignedCount) : std::span<const double>{},
        .peakBytes = ctx.peakMemoryBytes,
        .percentPerByte = smoothed.memoryPercentPerByte,
    };

    // Used, Shared, Private and Swap in bytes on an axis that scales to them (#1195, #1885), eased like
    // a rate axis and shared with their bars.
    const double memAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcOverviewMemory",
        Detail::memoryAxisDataMax(axis.timeData, axis.axisConfig.xMin, data.used, data.shared, smoothed, data.privateBytes, data.swapBytes),
        UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
    // Virtual size in bytes on its own right-hand axis (#992), eased like a rate axis and shared with
    // its bar, whose smoothed value it covers too.
    const double virtAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcOverviewMemory/Y2",
                                             Detail::virtualAxisDataMax(axis.timeData, axis.axisConfig.xMin, data.virtualBytes, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);

    const NowBarList memoryBars = memoryNowBars(smoothed, showShared, memAxisUpper, virtAxisUpper);

    auto memoryPlot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(
                UI::Widgets::rateHistoryConfigWithUpper(
                    "##ProcOverviewMemory", axis.axisConfig.xMin, axis.axisConfig.xMax, UI::Widgets::formatAxisBytes, memAxisUpper),
                fill.plotHeight()),
            ctx.historyGeneration));
        if (chart.active())
        {
            UI::Widgets::setupSecondaryRateAxis(virtAxisUpper, UI::Widgets::formatAxisBytes, theme.scheme().chartMemoryVirtual);
            UI::Widgets::drawCollectingHint(alignedCount);
            drawMemorySeries(axis, data);
        }
    };

    if (!UI::Widgets::beginChartCard("##ProcMemoryCard", fill.plotHeight()))
    {
        UI::Widgets::endChartCard("##ProcMemoryCard", fill.plotHeight());
        fill.addPlot();
        return;
    }
    (void) UI::Widgets::sectionHeader(ICON_FA_MEMORY, "Memory", {}, alignedCount);
    // Peak Memory is a line with a tooltip row but no bar; list it in the value strip too (#1193). Mem %,
    // the Processes table's column, is not plotted: a blank swatch, its share of RAM (#1878).
    std::array<UI::Widgets::ValueStripEntry, 6> memoryExtras{};
    std::size_t memoryExtraCount = 0;
    if (ctx.peakMemoryBytes > 0.0)
    {
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_PEAK_LABEL,
            .value = UI::Format::formatBytes(ctx.peakMemoryBytes),
            .color = theme.scheme().chartPeakLine,
        };
    }
    if (smoothed.memoryPercentPerByte > 0.0)
    {
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_PERCENT_LABEL,
            .value = UI::Format::percentOneDecimal(smoothed.residentBytes * smoothed.memoryPercentPerByte),
            .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
            .tooltip = "Its resident memory as a share of the system's RAM, as the Processes table's Mem % column",
        };
    }
    // Private and Swap, the Processes table's columns (#1885), where the platform reads them: lines
    // without bars, so listed in the strip like Peak, in their lines' colours. Windows fills neither --
    // its private bytes are the commit charge the Virtual line already draws, and it has no per-process
    // swap -- so both are left out there (#1879).
    if (ctx.hasPrivateBytes && ctx.snapshot != nullptr)
    {
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_PRIVATE_LABEL,
            .value = UI::Format::formatBytes(Domain::Numeric::toDouble(ctx.snapshot->privateBytes)),
            .color = theme.scheme().chartMemoryPrivate,
            .tooltip = "Memory no other process shares (RssAnon on Linux), as the Processes table's Private column",
        };
    }
    if (ctx.hasSwapBytes && ctx.snapshot != nullptr)
    {
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_SWAP_LABEL,
            .value = UI::Format::formatBytes(Domain::Numeric::toDouble(ctx.snapshot->swapBytes)),
            .color = theme.scheme().chartMemorySwap,
            .tooltip = "How much of it is swapped out (VmSwap on Linux), as the Processes table's Swap column",
        };
    }
    // PSS and USS (#1885), read for this process alone (ProcessSnapshot::memoryRollupRead): figures, not
    // plotted, so blank swatches like Mem %.
    if (ctx.snapshot != nullptr && ctx.snapshot->memoryRollupRead)
    {
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_PSS_LABEL,
            .value = UI::Format::formatBytes(Domain::Numeric::toDouble(ctx.snapshot->pssBytes)),
            .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
            .tooltip = "Proportional set size: its own pages plus its share of the pages it shares with other processes "
                       "(smaps_rollup Pss)",
        };
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_USS_LABEL,
            .value = UI::Format::formatBytes(Domain::Numeric::toDouble(ctx.snapshot->ussBytes)),
            .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
            .tooltip = "Unique set size: the memory freed if it exited, the pages no other process maps "
                       "(smaps_rollup Private_Clean + Private_Dirty)",
        };
    }
    else if (ctx.snapshot != nullptr && ctx.snapshot->memoryRollupDenied)
    {
        // Refused, not absent (#2309): another user's process, whose smaps_rollup needs ptrace read access.
        constexpr const char* DENIED_TOOLTIP = "Not readable: this is another user's process. TaskSmack needs administrator rights "
                                               "(or CAP_SYS_PTRACE) to read its PSS and USS";
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_PSS_LABEL, .value = "not readable", .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F), .tooltip = DENIED_TOOLTIP};
        memoryExtras.at(memoryExtraCount++) = UI::Widgets::ValueStripEntry{
            .label = MEM_USS_LABEL, .value = "not readable", .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F), .tooltip = DENIED_TOOLTIP};
    }
    const std::span<const UI::Widgets::ValueStripEntry> stripExtras(memoryExtras.data(), memoryExtraCount);
    renderHistoryWithNowBars("ProcessMemoryOverviewLayout",
                             fill.plotHeight(),
                             memoryPlot,
                             memoryBars,
                             {.minBarColumns = Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS, .stripExtras = stripExtras});
    UI::Widgets::endChartCard("##ProcMemoryCard", fill.plotHeight());
    fill.addPlot();
}

// Renders the thread/handle/page-fault history plot plus matching "now" bars. Every series has one
// value per timestamp (Detail::ProcessDetailsHistory), so every plotted value and tooltip lookup
// refers to the same point in time.
void ProcessDetailsCharts::renderThreadAndFaultHistory(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    if (history.empty())
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const std::size_t alignedCount = axis.alignedCount;
    const double xMin = axis.axisConfig.xMin;

    // Assigned field by field rather than designated-initialized: the GDI fields exist only on
    // Windows, where leaving them out of a designated initializer is an error.
    ResourceChartData data;
    data.threads = tailSpan(history.series(ProcessSeries::Threads), alignedCount);
    data.handles = tailSpan(history.series(ProcessSeries::Handles), alignedCount);
    data.faults = tailSpan(history.series(ProcessSeries::PageFaults), alignedCount);
#ifdef _WIN32
    // The GDI history can be shorter than the others, and ends at the same newest sample, so it starts
    // gdiTimeOffset timestamps in (#1001).
    const std::size_t gdiAlignedCount = std::min(alignedCount, history.series(ProcessSeries::GdiObjects).size());
    data.gdi = tailSpan(history.series(ProcessSeries::GdiObjects), gdiAlignedCount);
    data.gdiTimeOffset = Detail::seriesTimeOffset(alignedCount, data.gdi.size());
    data.hasGdiSamples = Detail::hasAnySample(data.gdi);
    const double threadSeriesMax = Detail::threadAxisDataMaxWithGdi(axis.timeData, xMin, data.threads, data.gdi, smoothed);
#else
    const double threadSeriesMax = Detail::threadAxisDataMax(axis.timeData, xMin, data.threads, smoothed);
#endif

    // Three scales: Threads (and GDI objects) on the left axis; page faults, a rate, on the right-hand
    // Y2 so a fault spike doesn't flatten the count lines (#1024); and handles/FDs on a right-hand Y3
    // outside it, as a process holds tens of times more of them than threads, which lay flat on the
    // baseline while the two shared an axis (#1916). Each bound covers every series drawn on its axis
    // and its bars' smoothed values (#1003, #1145).
    const ResourceAxisBounds bounds{
        .threads = UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults", threadSeriesMax, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT),
        .faults = UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults/Y2",
                                                       Detail::faultAxisDataMax(axis.timeData, xMin, data.faults, smoothed),
                                                       UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT),
        .handles = UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults/Y3",
                                                        Detail::handleAxisDataMax(axis.timeData, xMin, data.handles, smoothed),
                                                        UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT),
    };

    const NowBarList bars = resourceNowBars(smoothed, bounds, ctx.hasPageFaults);

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcThreadsFaults", xMin, axis.axisConfig.xMax, formatAxisLocalized, bounds.threads),
                                    fill.plotHeight()),
            ctx.historyGeneration));
        if (chart.active())
        {
            UI::Widgets::setupSecondaryRateAxis(bounds.faults, formatAxisLocalized, theme.scheme().chartPageFaults);
            UI::Widgets::setupRightRateAxis(ImAxis_Y3, bounds.handles, formatAxisLocalized, theme.scheme().chartHandles);
            UI::Widgets::drawCollectingHint(alignedCount);
            drawResourceSeries(axis, data);
        }
    };

    if (UI::Widgets::beginChartCard("##ProcResourcesCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_GEARS, "Resources", {}, alignedCount);
        // The cumulative count, the Processes table's Page Faults column, beside the rate the chart plots:
        // not plotted, so a blank swatch (#1878).
        const std::array faultsTotal{UI::Widgets::ValueStripEntry{
            .label = FAULTS_TOTAL_LABEL,
            .value = UI::Format::formatIntLocalized(ctx.snapshot != nullptr ? ctx.snapshot->pageFaults : std::uint64_t{0}),
            .color = ImVec4(0.0F, 0.0F, 0.0F, 0.0F),
            .tooltip = "Page faults since the process started, as the Processes table's Page Faults column",
        }};
        const std::span<const UI::Widgets::ValueStripEntry> resourceExtras = (ctx.hasPageFaults && ctx.snapshot != nullptr)
                                                                               ? std::span<const UI::Widgets::ValueStripEntry>(faultsTotal)
                                                                               : std::span<const UI::Widgets::ValueStripEntry>{};
        renderHistoryWithNowBars("ProcessResourceHistory",
                                 fill.plotHeight(),
                                 plot,
                                 bars,
                                 {.minBarColumns = Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS, .stripExtras = resourceExtras});
    }
    UI::Widgets::endChartCard("##ProcResourcesCard", fill.plotHeight());
    fill.addPlot();
}

void ProcessDetailsCharts::renderPowerUsage(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    const bool hasCurrent = ctx.snapshot->powerWatts > 0.0;
    if (history.empty() && !hasCurrent)
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const auto powerData = tailSpan(history.series(ProcessSeries::Power), axis.alignedCount);

    // Use smoothed value for NowBar; the axis covers it too, so the bar is not clamped while it eases
    // down from a peak that has just left the window (#1145).
    const double powerAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcPowerHistory",
                                             Detail::powerAxisDataMax(axis.timeData, axis.axisConfig.xMin, powerData, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_WATTS);

    const NowBar powerBar{.valueText = UI::Format::formatPowerOrZero(smoothed.powerWatts),
                          .label = POWER_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.powerWatts, powerAxisUpper),
                          .color = theme.scheme().chartPower};

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcPowerHistory", axis.axisConfig.xMin, axis.axisConfig.xMax, formatAxisWatts, powerAxisUpper),
                                    fill.plotHeight()),
            ctx.historyGeneration));
        if (!chart.active())
        {
            return;
        }
        UI::Widgets::drawCollectingHint(powerData.size());
        if (powerData.empty())
        {
            ImPlot::PlotDummy("Power");
            return;
        }
        // The role's own fill, as on the system Power chart, so charts.power_fill applies on every screen.
        plotLineWithFill(POWER_LABEL,
                         axis.timeData.data(),
                         powerData.data(),
                         UI::Format::checkedCount(powerData.size()),
                         theme.scheme().chartPower,
                         theme.scheme().chartPowerFill);
        if (const auto idx = hoveredSampleIndex(axis.timeData, powerData.size()))
        {
            const std::array rows{UI::Widgets::TooltipRow{
                .label = POWER_LABEL,
                .color = theme.scheme().chartPower,
                .value = UI::Widgets::formatSampleOrNA(powerData[*idx], [](double v) { return UI::Format::formatPowerOrZero(v); })}};
            UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
        }
    };

    if (UI::Widgets::beginChartCard("##ProcPowerCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_BOLT, "Power Usage", {}, axis.alignedCount);
        renderHistoryWithNowBars(
            "ProcessPowerHistory", fill.plotHeight(), plot, {powerBar}, {.minBarColumns = Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS});
    }
    UI::Widgets::endChartCard("##ProcPowerCard", fill.plotHeight());
    fill.addPlot();
}

void ProcessDetailsCharts::renderNetworkTab(const ProcessChartContext& ctx)
{
    if (ctx.history == nullptr || ctx.smoothed == nullptr || ctx.snapshot == nullptr)
    {
        return;
    }
    // Readings only: every sample adds a point, a gap where there was no reading, so a history that
    // is merely non-empty is not data (#1210).
    if (!Detail::hasNetworkTabData(*ctx.history))
    {
        UI::Widgets::renderEmptyState(ICON_FA_NETWORK_WIRED "  No network or disk I/O yet",
                                      "Disk and network rates for this process appear here once they have been sampled.");
        return;
    }
    UI::Widgets::FillPlotLayout fill(m_NetworkFill);
    const UI::Widgets::AlignedChartStack alignedCharts("##ProcNetworkCharts"); // #1206
    // Render I/O stats first (at the top)
    renderIoStats(ctx, fill);
    renderNetworkStats(ctx, fill);
}

void ProcessDetailsCharts::renderIoStats(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    if (history.empty())
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const auto readData = tailSpan(history.series(ProcessSeries::IoRead), axis.alignedCount);
    const auto writeData = tailSpan(history.series(ProcessSeries::IoWrite), axis.alignedCount);

    // Unreadable I/O counters (#1110) show N/A, as their lines show a gap.
    const bool ioAvailable = smoothed.ioAvailable;

    // Compare the smoothed current rates with the visible history when scaling the NowBars, so either
    // a visible or newly observed peak remains representable, and a bar still easing down from a peak
    // that has just left the window is not clamped to full height (#1145).
    const double ioAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcIoHistory",
                                             Detail::ioAxisDataMax(axis.timeData, axis.axisConfig.xMin, readData, writeData, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const NowBar readBar{.valueText = Detail::rateTextOrNA(ioAvailable, smoothed.ioReadBytesPerSec),
                         .label = IO_READ_LABEL,
                         .tooltipText = {},
                         .value01 = ioAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.ioReadBytesPerSec, ioAxisUpper) : 0.0,
                         .color = theme.scheme().chartIo,
                         .marker = ImPlotMarker_None,
                         .available = ioAvailable}; // N/A is drawn hatched, not as a 0 bar (#1918)
    const NowBar writeBar{.valueText = Detail::rateTextOrNA(ioAvailable, smoothed.ioWriteBytesPerSec),
                          .label = IO_WRITE_LABEL,
                          .tooltipText = {},
                          .value01 = ioAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.ioWriteBytesPerSec, ioAxisUpper) : 0.0,
                          .color = theme.scheme().chartIoWrite,
                          .marker = ImPlotMarker_None,
                          .available = ioAvailable};
    // Nothing to draw and no reading now: say so over the plot, rather than an empty axis (#1918).
    const char* ioNotice = Detail::rateChartNotice(ioAvailable, readData, writeData, Detail::IO_NOT_AVAILABLE_NOTICE);

    // Keep the plot and its hover tooltip together: both consume the same aligned vectors, and the
    // lambda is rendered alongside the matching NowBars below.
    auto plot = [&]()
    {
        drawTwoSeriesPlot(
            "##ProcIoHistory",
            axis,
            ioAxisUpper,
            fill.plotHeight(),
            ctx.historyGeneration,
            BYTES_PER_SEC_UNITS,
            {.label = IO_READ_LABEL, .data = readData, .color = theme.scheme().chartIo, .fill = theme.scheme().chartIoFill},
            {.label = IO_WRITE_LABEL, .data = writeData, .color = theme.scheme().chartIoWrite, .fill = theme.scheme().chartIoWriteFill},
            ioNotice);
    };

    if (UI::Widgets::beginChartCard("##ProcIoCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_HARD_DRIVE, "I/O Statistics", {}, axis.alignedCount);
        renderHistoryWithNowBars("ProcessIoHistory",
                                 fill.plotHeight(),
                                 plot,
                                 {readBar, writeBar},
                                 {.minBarColumns = Detail::PROCESS_NETWORK_IO_NOW_BAR_COLUMNS});
    }
    UI::Widgets::endChartCard("##ProcIoCard", fill.plotHeight());
    fill.addPlot();
}

void ProcessDetailsCharts::renderNetworkStats(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    if (history.empty())
    {
        return;
    }
    const auto& theme = UI::Theme::get();
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const auto sentData = tailSpan(history.series(ProcessSeries::NetSent), axis.alignedCount);
    const auto recvData = tailSpan(history.series(ProcessSeries::NetReceived), axis.alignedCount);

    // Network counters that couldn't be attributed to the process (#1110) show N/A, as their lines
    // show a gap.
    const bool netAvailable = smoothed.networkAvailable;

    // Scale the NowBars against both the visible peak and the smoothed current values, so a new
    // traffic burst cannot exceed the normalized range, nor a bar still easing down from a peak that
    // has just left the window (#1145).
    const double netAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcNetworkHistory",
                                             Detail::networkAxisDataMax(axis.timeData, axis.axisConfig.xMin, sentData, recvData, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

    const NowBar sentBar{.valueText = Detail::rateTextOrNA(netAvailable, smoothed.netSentBytesPerSec),
                         .label = NET_SENT_LABEL,
                         .tooltipText = {},
                         .value01 = netAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.netSentBytesPerSec, netAxisUpper) : 0.0,
                         .color = theme.scheme().chartNetTx,
                         .marker = ImPlotMarker_None,
                         .available = netAvailable}; // N/A is drawn hatched, not as a 0 bar (#1918)
    const NowBar recvBar{.valueText = Detail::rateTextOrNA(netAvailable, smoothed.netRecvBytesPerSec),
                         .label = NET_RECV_LABEL,
                         .tooltipText = {},
                         .value01 = netAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.netRecvBytesPerSec, netAxisUpper) : 0.0,
                         .color = theme.scheme().chartNetRx,
                         .marker = ImPlotMarker_None,
                         .available = netAvailable};
    // Nothing to draw and no reading now: say so over the plot, rather than an empty axis (#1918).
    const char* netNotice = Detail::rateChartNotice(netAvailable, sentData, recvData, Detail::NETWORK_NOT_AVAILABLE_NOTICE);

    // The plot lambda owns rendering and hover lookup over the same aligned buffers;
    // renderHistoryWithNowBars composes it with the summary bars. The tooltip's rates are averages
    // since the process was first seen, as the chart's heading says.
    auto plot = [&]()
    {
        drawTwoSeriesPlot(
            "##ProcNetworkHistory",
            axis,
            netAxisUpper,
            fill.plotHeight(),
            ctx.historyGeneration,
            BYTES_PER_SEC_UNITS,
            {.label = NET_SENT_LABEL, .data = sentData, .color = theme.scheme().chartNetTx, .fill = theme.scheme().chartNetTxFill},
            {.label = NET_RECV_LABEL, .data = recvData, .color = theme.scheme().chartNetRx, .fill = theme.scheme().chartNetRxFill},
            netNotice);
    };

    // The heading's tooltip is shown after the chart: its value strip is placed beside the heading,
    // the item drawn just before it, so nothing else is submitted between the two. It is the header's
    // only tooltip, so it carries the sample count too: a second one in the same frame would replace
    // the other (#1200 review).
    const bool cardVisible = UI::Widgets::beginChartCard("##ProcNetworkCard", fill.plotHeight());
    const bool headingHovered = cardVisible && UI::Widgets::sectionHeader(ICON_FA_NETWORK_WIRED, "Network");
    if (cardVisible)
    {
        renderHistoryWithNowBars("ProcessNetworkHistory",
                                 fill.plotHeight(),
                                 plot,
                                 {sentBar, recvBar},
                                 {.minBarColumns = Detail::PROCESS_NETWORK_IO_NOW_BAR_COLUMNS});
    }
    if (headingHovered && ImGui::BeginTooltip())
    {
        ImGui::TextUnformatted("Network bytes/sec between readings of the process's open connections. A refresh that reuses a cached "
                               "reading shows the last rate.");
        std::array<char, 64> samplesText{};
        const std::string_view samples = UI::ChromeLayout::formatSampleCount(samplesText, axis.alignedCount);
        ImGui::TextUnformatted(samples.data(), samples.data() + samples.size());
        ImGui::EndTooltip();
    }
    UI::Widgets::endChartCard("##ProcNetworkCard", fill.plotHeight());
    fill.addPlot();
}

void ProcessDetailsCharts::renderGpuTab(const ProcessChartContext& ctx)
{
    if (ctx.history == nullptr || ctx.smoothed == nullptr || ctx.snapshot == nullptr)
    {
        return;
    }
    // GPU only: a process's NPU use is on the NPU tab (#1869). A failed read is not "not available on
    // this system" (#1210).
    const Detail::GpuTabContent gpuContent = Detail::gpuTabContentFor(*ctx.snapshot, *ctx.history, ctx.rateReadings.gpuSupported);
    if (renderDeviceTabEmptyState(gpuContent, GPU_TAB_EMPTY_TEXT, ctx.maxHistorySeconds, m_NoGpuUsageDetail, m_NoGpuUsageDetailKey))
    {
        return;
    }
    // The two history charts share the tab's height, like the other tabs' charts (#959). The
    // metrics table and per-GPU breakdown above them count as non-plot.
    UI::Widgets::FillPlotLayout fill(m_GpuFill);
    const UI::Widgets::AlignedChartStack alignedCharts("##ProcGpuCharts"); // #1206
    renderGpuUsage(ctx, fill);
}

void ProcessDetailsCharts::renderNpuTab(const ProcessChartContext& ctx)
{
    if (ctx.history == nullptr || ctx.smoothed == nullptr || ctx.snapshot == nullptr)
    {
        return;
    }
    // Decided as the GPU tab's, from the process's NPU figures and history (#1869).
    const Detail::GpuTabContent npuContent = Detail::npuTabContentFor(*ctx.snapshot, *ctx.history, ctx.rateReadings.npuSupported);
    if (renderDeviceTabEmptyState(npuContent, NPU_TAB_EMPTY_TEXT, ctx.maxHistorySeconds, m_NoNpuUsageDetail, m_NoNpuUsageDetailKey))
    {
        return;
    }
    // As the GPU tab: the usage card counts as non-plot, and the two charts share the rest (#959, #1206).
    UI::Widgets::FillPlotLayout fill(m_NpuFill);
    const UI::Widgets::AlignedChartStack alignedCharts("##ProcNpuCharts");
    renderNpuUsageCard(ctx);
    renderNpuHistoryGraphs(ctx, fill);
}

void ProcessDetailsCharts::renderGpuUsage(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& proc = *ctx.snapshot;

    // Throttle debug logging to avoid per-frame spam: only log when GPU data changes or on first
    // render of a new process. Member variables keep the tracking per view (not static).
    if (proc.pid != m_LastGpuLogPid || proc.gpuMemoryBytes != m_LastGpuLogMemoryBytes)
    {
        spdlog::debug("renderGpuUsage: PID {} util={:.1f}%, mem={}, devices='{}'",
                      proc.pid,
                      proc.gpuUtilPercent,
                      proc.gpuMemoryBytes,
                      proc.gpuDevices);

        m_LastGpuLogPid = proc.pid;
        m_LastGpuLogMemoryBytes = proc.gpuMemoryBytes;
    }

    // The current values as a card, as CPU Details is on System › Overview (#1594).
    if (UI::Widgets::beginCompactCard("##ProcGpuUsageCard", ImVec2(-FLT_MIN, 0.0F), ImGuiChildFlags_AutoResizeY))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_MICROCHIP, "GPU Usage");
        renderGpuCurrentMetricsTable(ctx);
        // With one GPU the breakdown repeats the table above (#1207).
        if (Detail::shouldShowPerGpuBreakdown(proc.perGpuUsage.size()))
        {
            ImGui::Separator();
            renderPerGpuBreakdown(ctx);
        }
    }
    UI::Widgets::endCard();

    renderGpuHistoryGraphs(ctx, fill);
}

// Renders the current-value GPU metrics table (utilization, memory, devices, engines,
// encoder/decoder) for the selected process.
void ProcessDetailsCharts::renderGpuCurrentMetricsTable(const ProcessChartContext& ctx)
{
    const auto& proc = *ctx.snapshot;
    const auto& smoothed = *ctx.smoothed;
    const auto& theme = UI::Theme::get();

    // Every label this table can show. The label column is measured from them (#966), so a label
    // added below must be added here too -- which is why the rows use these constants rather than
    // repeating the strings.
    constexpr const char* LABEL_UTILIZATION = "GPU Utilization:";
    constexpr const char* LABEL_MEMORY = "GPU Memory:";
    constexpr const char* LABEL_DEDICATED = "  Dedicated:";
    constexpr const char* LABEL_SHARED = "  Shared:";
    constexpr const char* LABEL_DEVICES = "GPU Device(s):";
    constexpr const char* LABEL_ENGINES = "Active Engines:";
    constexpr const char* LABEL_ENCODER = "Video Encoder:";
    constexpr const char* LABEL_DECODER = "Video Decoder:";
    constexpr auto LABELS = std::to_array<const char*>(
        {LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEDICATED, LABEL_SHARED, LABEL_DEVICES, LABEL_ENGINES, LABEL_ENCODER, LABEL_DECODER});

    const auto row = labelValueRow;

    // Current GPU metrics
    if (!ImGui::BeginTable("GPUCurrentMetrics", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::Widgets::measureLabelColumnWidth(LABELS));
    // An explicit weight: left to default, a SizingStretchProp column's weight is its content width,
    // which is 0 on the table's first frame, and the 0/0 left a NaN in the auto-sized GPU Usage card's
    // content size (UBSan, #1594).
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0F);

    const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
    row(LABEL_UTILIZATION, Detail::gpuUtilizationText(smoothed.gpuUtilAvailable, smoothed.gpuUtilPercent), &gpuUtilColor);

    const ImVec4 gpuMemColor = theme.scheme().gpuMemory;
    row(LABEL_MEMORY, smoothed.gpuMemoryAvailable ? UI::Format::formatBytes(smoothed.gpuMemoryBytes) : std::string("N/A"), &gpuMemColor);

    // GPU Memory counts what each GPU's "used" figure on the GPU tab counts (#1164); both kinds are
    // listed beneath it whenever that total doesn't already show them.
    if (Detail::showsGpuMemoryKinds(proc.gpuMemoryBytes, proc.gpuDedicatedMemoryBytes, proc.gpuSharedMemoryBytes))
    {
        row(LABEL_DEDICATED, UI::Format::formatBytes(Domain::Numeric::toDouble(proc.gpuDedicatedMemoryBytes)), &gpuMemColor);
        row(LABEL_SHARED, UI::Format::formatBytes(Domain::Numeric::toDouble(proc.gpuSharedMemoryBytes)), &gpuMemColor);
    }

    if (!proc.gpuDevices.empty())
    {
        row(LABEL_DEVICES, proc.gpuDevices, nullptr);
    }
    if (!proc.gpuEngines.empty())
    {
        row(LABEL_ENGINES, Detail::joinWithCommas(proc.gpuEngines), nullptr);
    }

    // Encoder/Decoder utilization
    if (proc.gpuEncoderUtil > 0.0)
    {
        const ImVec4 encColor = theme.scheme().gpuEncoder;
        row(LABEL_ENCODER, UI::Format::percentOneDecimal(proc.gpuEncoderUtil), &encColor);
    }
    if (proc.gpuDecoderUtil > 0.0)
    {
        const ImVec4 decColor = theme.scheme().gpuDecoder;
        row(LABEL_DECODER, UI::Format::percentOneDecimal(proc.gpuDecoderUtil), &decColor);
    }

    ImGui::EndTable();
}

// Renders a collapsible per-GPU breakdown (utilization, memory, engines) for each entry in
// proc.perGpuUsage. No-op if that list is empty, regardless of how many GPUs the system has; the caller
// skips it for a single GPU (#1207).
void ProcessDetailsCharts::renderPerGpuBreakdown(const ProcessChartContext& ctx)
{
    const auto& proc = *ctx.snapshot;
    const auto& theme = UI::Theme::get();

    // Per-GPU breakdown if available
    if (proc.perGpuUsage.empty())
    {
        return;
    }
    ImGui::Text("Per-GPU Breakdown:");
    ImGui::Spacing();

    const ImVec4 gpuUtilColor = theme.scheme().gpuUtilization;
    const ImVec4 gpuMemColor = theme.scheme().gpuMemory;

    // As in renderGpuCurrentMetricsTable(): the label column is measured from these (#966).
    constexpr const char* LABEL_UTILIZATION = "Utilization:";
    constexpr const char* LABEL_MEMORY = "Memory:";
    constexpr const char* LABEL_DEDICATED = "  Dedicated:";
    constexpr const char* LABEL_SHARED = "  Shared:";
    constexpr const char* LABEL_ENGINES = "Engines:";
    constexpr auto LABELS = std::to_array<const char*>({LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEDICATED, LABEL_SHARED, LABEL_ENGINES});
    const float labelColumnWidth = UI::Widgets::measureLabelColumnWidth(LABELS);

    const auto row = labelValueRow;

    for (const auto& gpuUsage : proc.perGpuUsage)
    {
        // Same words as the system GPU tab (GpuSection.cpp), so one adapter is not described two ways
        // depending on which tab is open (#963).
        const std::string gpuLabel =
            std::format("{} {} [{}]", ICON_FA_MICROCHIP, gpuUsage.gpuName, gpuUsage.isIntegrated ? "Shared Memory" : "Discrete");
        if (!ImGui::CollapsingHeader(gpuLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
        {
            continue;
        }
        ImGui::Indent();

        if (ImGui::BeginTable("PerGPUMetrics", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, labelColumnWidth);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0F); // As above: no first-frame NaN

            row(LABEL_UTILIZATION, Detail::gpuUtilizationText(ctx.rateReadings.gpuUtilization, gpuUsage.utilPercent), &gpuUtilColor);
            row(LABEL_MEMORY, UI::Format::formatBytes(static_cast<double>(gpuUsage.memoryBytes)), &gpuMemColor);
            // As in renderGpuCurrentMetricsTable(): both kinds whenever the total doesn't show them (#1164).
            if (Detail::showsGpuMemoryKinds(gpuUsage.memoryBytes, gpuUsage.dedicatedMemoryBytes, gpuUsage.sharedMemoryBytes))
            {
                row(LABEL_DEDICATED, UI::Format::formatBytes(Domain::Numeric::toDouble(gpuUsage.dedicatedMemoryBytes)), &gpuMemColor);
                row(LABEL_SHARED, UI::Format::formatBytes(Domain::Numeric::toDouble(gpuUsage.sharedMemoryBytes)), &gpuMemColor);
            }
            if (!gpuUsage.engines.empty())
            {
                row(LABEL_ENGINES, Detail::joinWithCommas(gpuUsage.engines), nullptr);
            }

            ImGui::EndTable();
        }

        ImGui::Unindent();
        ImGui::Spacing();
    }
}

// Renders the GPU utilization and memory history charts: drawn from the start, with the collecting
// hint until samples arrive, like every other chart (#1013); this was a line of text until there was
// history.
void ProcessDetailsCharts::renderGpuHistoryGraphs(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    const auto& theme = UI::Theme::get();

    // Every series has one value per timestamp (Detail::ProcessDetailsHistory), so none can be read
    // past its end (#1149).
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    const auto gpuMemVec = tailSpan(history.series(ProcessSeries::GpuMemory), axis.alignedCount);

    // GPU Utilization graph (percent metric: locked 0-100 axis with percent formatter)
    renderUtilizationChartCard({.plotId = "##GPUUtilPlot",
                                .cardId = "##ProcGpuUtilCard",
                                .layoutId = "ProcessGPUUtilHistory",
                                .heading = "GPU Utilization History",
                                .nowBarColumns = Detail::PROCESS_GPU_NOW_BAR_COLUMNS},
                               axis,
                               tailSpan(history.series(ProcessSeries::GpuUtil), axis.alignedCount),
                               smoothed.gpuUtilAvailable,
                               smoothed.gpuUtilPercent,
                               ctx.historyGeneration,
                               fill);

    // GPU Memory graph. One upper bound for its axis and its bar, so they agree (#1003), from the
    // samples in the window, not the one trimming keeps left of it (#1145), and the bar's smoothed
    // value, which can still be easing down from a peak that has just left it.
    const double gpuMemAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##GPUMemPlot",
                                             Detail::gpuMemoryAxisDataMax(axis.timeData, axis.axisConfig.xMin, gpuMemVec, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
    auto plotGpuMem = [&]()
    {
        drawGpuHistoryPlot(
            UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(
                    UI::Widgets::rateHistoryConfigWithUpper(
                        "##GPUMemPlot", axis.axisConfig.xMin, axis.axisConfig.xMax, UI::Widgets::formatAxisBytes, gpuMemAxisUpper),
                    fill.plotHeight()),
                ctx.historyGeneration),
            axis,
            gpuMemVec,
            GPU_MEMORY_LABEL,
            "GPU Memory",
            theme.scheme().gpuMemory,
            theme.scheme().gpuMemoryFill,
            bytesOrNA);
    };

    // The memory chart's now bar
    const NowBar gpuMemBar{
        .valueText = smoothed.gpuMemoryAvailable ? UI::Format::formatBytes(smoothed.gpuMemoryBytes) : std::string("N/A"),
        .label = GPU_MEMORY_LABEL,
        .tooltipText = {},
        .value01 = smoothed.gpuMemoryAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.gpuMemoryBytes, gpuMemAxisUpper) : 0.0,
        .color = theme.scheme().gpuMemory,
        .marker = ImPlotMarker_None,
        .available = smoothed.gpuMemoryAvailable, // N/A is drawn hatched, not as a 0 bar (#1935)
    };

    // In its own card, as the utilization chart above (#1594).
    if (UI::Widgets::beginChartCard("##ProcGpuMemCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_CHART_LINE, "GPU Memory History", {}, axis.alignedCount);
        renderHistoryWithNowBars(
            "ProcessGPUMemHistory", fill.plotHeight(), plotGpuMem, {gpuMemBar}, {.minBarColumns = Detail::PROCESS_GPU_NOW_BAR_COLUMNS});
    }
    UI::Widgets::endChartCard("##ProcGpuMemCard", fill.plotHeight());
    fill.addPlot();
}

// Renders the NPU tab's usage card (#1799, #1869): the process's busiest NPU's or compute accelerator's
// utilization, its memory on them (dedicated and shared beneath where the total doesn't show them),
// the devices and the active engines. Laid out as the GPU tab's usage card.
void ProcessDetailsCharts::renderNpuUsageCard(const ProcessChartContext& ctx)
{
    const auto& proc = *ctx.snapshot;
    const auto& smoothed = *ctx.smoothed;
    const auto& theme = UI::Theme::get();

    // As in renderGpuCurrentMetricsTable(): the label column is measured from these (#966).
    constexpr const char* LABEL_UTILIZATION = "NPU Utilization:";
    constexpr const char* LABEL_MEMORY = "NPU Memory:";
    constexpr const char* LABEL_DEDICATED = "  Dedicated:";
    constexpr const char* LABEL_SHARED = "  Shared:";
    constexpr const char* LABEL_DEVICES = "NPU Device(s):";
    constexpr const char* LABEL_ENGINES = "Active Engines:";
    constexpr auto LABELS =
        std::to_array<const char*>({LABEL_UTILIZATION, LABEL_MEMORY, LABEL_DEDICATED, LABEL_SHARED, LABEL_DEVICES, LABEL_ENGINES});
    const auto row = labelValueRow;

    if (UI::Widgets::beginCompactCard("##ProcNpuUsageCard", ImVec2(-FLT_MIN, 0.0F), ImGuiChildFlags_AutoResizeY))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_BRAIN, "NPU Usage");
        if (ImGui::BeginTable("NPUCurrentMetrics", 2, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::Widgets::measureLabelColumnWidth(LABELS));
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0F); // As the GPU table: no first-frame NaN

            const ImVec4 utilColor = theme.scheme().gpuUtilization;
            row(LABEL_UTILIZATION, Detail::gpuUtilizationText(smoothed.npuUtilAvailable, smoothed.npuUtilPercent), &utilColor);
            const ImVec4 memColor = theme.scheme().gpuMemory;
            row(LABEL_MEMORY,
                smoothed.npuMemoryAvailable ? UI::Format::formatBytes(smoothed.npuMemoryBytes) : std::string("N/A"),
                &memColor);
            if (Detail::showsGpuMemoryKinds(proc.npuMemoryBytes, proc.npuDedicatedMemoryBytes, proc.npuSharedMemoryBytes))
            {
                row(LABEL_DEDICATED, UI::Format::formatBytes(Domain::Numeric::toDouble(proc.npuDedicatedMemoryBytes)), &memColor);
                row(LABEL_SHARED, UI::Format::formatBytes(Domain::Numeric::toDouble(proc.npuSharedMemoryBytes)), &memColor);
            }
            if (!proc.npuDevices.empty())
            {
                row(LABEL_DEVICES, proc.npuDevices, nullptr);
            }
            if (!proc.npuEngines.empty())
            {
                row(LABEL_ENGINES, Detail::joinWithCommas(proc.npuEngines), nullptr);
            }
            ImGui::EndTable();
        }
    }
    UI::Widgets::endCard();
}

// Renders the NPU tab's utilization and memory history charts (#1869), as the GPU tab's: the same
// utilization chart, and a memory chart with dedicated and shared memory as two series, since an NPU
// that maps system RAM (Intel AI Boost) has only shared memory.
void ProcessDetailsCharts::renderNpuHistoryGraphs(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& history = *ctx.history;
    const auto& smoothed = *ctx.smoothed;
    const auto& scheme = UI::Theme::get().scheme();

    // Every series has one value per timestamp (Detail::ProcessDetailsHistory), as on the GPU tab (#1149).
    const ChartTimeAxis axis = chartTimeAxis(history, ctx.maxHistorySeconds);
    renderUtilizationChartCard({.plotId = "##NPUUtilPlot",
                                .cardId = "##ProcNpuUtilCard",
                                .layoutId = "ProcessNPUUtilHistory",
                                .heading = "NPU Utilization History",
                                .nowBarColumns = Detail::PROCESS_NPU_NOW_BAR_COLUMNS},
                               axis,
                               tailSpan(history.series(ProcessSeries::NpuUtil), axis.alignedCount),
                               smoothed.npuUtilAvailable,
                               smoothed.npuUtilPercent,
                               ctx.historyGeneration,
                               fill);

    const auto dedicated = tailSpan(history.series(ProcessSeries::NpuDedicatedMemory), axis.alignedCount);
    const auto shared = tailSpan(history.series(ProcessSeries::NpuSharedMemory), axis.alignedCount);
    // One upper bound for the axis and its bars, so they agree (#1003), as the GPU Memory chart's.
    const double memAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##NPUMemPlot",
                                             Detail::npuMemoryAxisDataMax(axis.timeData, axis.axisConfig.xMin, dedicated, shared, smoothed),
                                             UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES);
    const auto memBar = [&smoothed, memAxisUpper](const char* label, double bytes, const ImVec4& color)
    {
        return NowBar{
            .valueText = smoothed.npuMemoryAvailable ? UI::Format::formatBytes(bytes) : std::string("N/A"),
            .label = label,
            .tooltipText = {},
            .value01 = smoothed.npuMemoryAvailable ? UI::Widgets::normalizeToUnitInterval(bytes, memAxisUpper) : 0.0,
            .color = color,
            .marker = ImPlotMarker_None,
            .available = smoothed.gpuMemoryAvailable, // N/A is drawn hatched, not as a 0 bar (#1935)
        };
    };
    const NowBar dedicatedBar = memBar(NPU_DEDICATED_LABEL, smoothed.npuDedicatedMemoryBytes, scheme.gpuMemory);
    const NowBar sharedBar = memBar(NPU_SHARED_LABEL, smoothed.npuSharedMemoryBytes, scheme.chartMemoryShared);
    auto plotMem = [&]()
    {
        drawTwoSeriesPlot(
            "##NPUMemPlot",
            axis,
            memAxisUpper,
            fill.plotHeight(),
            ctx.historyGeneration,
            BYTES_UNITS,
            {.label = NPU_DEDICATED_LABEL, .data = dedicated, .color = scheme.gpuMemory, .fill = scheme.gpuMemoryFill},
            {.label = NPU_SHARED_LABEL, .data = shared, .color = scheme.chartMemoryShared, .fill = scheme.chartMemorySharedFill});
    };
    if (UI::Widgets::beginChartCard("##ProcNpuMemCard", fill.plotHeight()))
    {
        (void) UI::Widgets::sectionHeader(ICON_FA_CHART_LINE, "NPU Memory History", {}, axis.alignedCount);
        renderHistoryWithNowBars("ProcessNPUMemHistory",
                                 fill.plotHeight(),
                                 plotMem,
                                 {dedicatedBar, sharedBar},
                                 {.minBarColumns = Detail::PROCESS_NPU_NOW_BAR_COLUMNS});
    }
    UI::Widgets::endChartCard("##ProcNpuMemCard", fill.plotHeight());
    fill.addPlot();
}

} // namespace App
