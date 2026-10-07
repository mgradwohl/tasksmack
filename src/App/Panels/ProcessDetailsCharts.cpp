#include "ProcessDetailsCharts.h"

#include "Domain/Numeric.h"
#include "Domain/ProcessSnapshot.h"
#include "ProcessDetailsChartHelpers.h"
#include "ProcessDetailsHistory.h"
#include "ProcessDetailsPanel_GpuHelpers.h"
#include "ProcessSmoothedUsage.h"
#include "UI/ChartWidgets.h"
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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
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
// A series on a chart's right-hand axis ends in " →", pointing at it (setupSecondaryRateAxis(), #1206); in
// its value-strip entry and tooltip rows the mark follows the value (SECONDARY_AXIS_MARK, #1300).
constexpr const char* MEM_VIRTUAL_LABEL = "Virtual →";
// A peak is named "Peak " plus the series it tracks: this chart's series is "Memory", as the
// System chart's "Peak Used" tracks "Used" (#1342). The Processes table keeps its short "Peak Mem"
// header for width, as it does "Mem %".
constexpr const char* MEM_PEAK_LABEL = "Peak Memory";
constexpr const char* THREADS_LABEL = "Threads";
constexpr const char* FAULTS_LABEL = "Page Faults →"; // Its values carry the "/s" ("12.0/s"), #1202
#ifdef _WIN32
constexpr const char* GDI_LABEL = "GDI Objects";
constexpr const char* HANDLE_LABEL = "Handles";
#else
constexpr const char* HANDLE_LABEL = "FDs";
#endif
constexpr const char* IO_READ_LABEL = "Read";
constexpr const char* IO_WRITE_LABEL = "Write";
constexpr const char* NET_SENT_LABEL = "Sent";
constexpr const char* NET_RECV_LABEL = "Received";
constexpr const char* POWER_LABEL = "Power";
constexpr const char* GPU_UTIL_LABEL = "Utilization";
constexpr const char* GPU_MEMORY_LABEL = "Memory";

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
    axis.axisConfig = makeTimeAxisConfig(timestamps, maxHistorySeconds, 0.0);
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

/// Draws a two-series rate chart -- @p primary at the primary series' weight, @p secondary as the
/// first secondary -- on a bytes/sec axis from 0 to @p upper, with its hover tooltip.
void drawTwoRatePlot(const char* plotId,
                     const ChartTimeAxis& axis,
                     double upper,
                     float plotHeight,
                     std::uint64_t historyGeneration,
                     const RateSeries& primary,
                     const RateSeries& secondary)
{
    const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
        UI::Widgets::withHeight(
            UI::Widgets::rateHistoryConfigWithUpper(plotId, axis.axisConfig.xMin, axis.axisConfig.xMax, formatAxisBytesPerSec, upper),
            plotHeight),
        historyGeneration));
    if (!chart.active())
    {
        return;
    }
    UI::Widgets::drawCollectingHint(axis.alignedCount);
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
            UI::Widgets::TooltipRow{
                .label = primary.label, .color = primary.color, .value = UI::Format::formatBytesPerSecOrNA(primary.data[*idx])},
            UI::Widgets::TooltipRow{
                .label = secondary.label, .color = secondary.color, .value = UI::Format::formatBytesPerSecOrNA(secondary.data[*idx])},
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
                        .color = scheme.chartCpu,
                        .value = App::Detail::formatBytesWithRamShare(data.shared[*idxVal], data.percentPerByte)});
    }
    if (*idxVal < data.virtualBytes.size())
    {
        rows.push_back(
            {.label = MEM_VIRTUAL_LABEL,
             .color = scheme.chartIo,
             .value = UI::Widgets::formatSampleOrNA(data.virtualBytes[*idxVal], [](double v) { return UI::Format::formatBytes(v); })});
    }
    if (data.peakBytes > 0.0)
    {
        // The line's colour: this row was textWarning, which matched nothing (#1005).
        rows.push_back({.label = MEM_PEAK_LABEL, .color = scheme.chartPeakLine, .value = UI::Format::formatBytes(data.peakBytes)});
    }
    UI::Widgets::renderHistoryTooltip(axis.timeData[*idxVal], rows);
}

/// The Memory chart's lines inside its plot: the peak reference line, Used and Shared on the left
/// axis, Virtual on the right, and the tooltip.
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
                   scheme.chartCpu,
                   scheme.chartCpuFill,
                   seriesStyle(SeriesRole::Secondary, 0));
    }

    if (!data.virtualBytes.empty())
    {
        // Line only: a fill on its own scale would cover the Used and Shared areas.
        ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
        plotSeries(MEM_VIRTUAL_LABEL,
                   axis.timeData.data(),
                   data.virtualBytes.data(),
                   UI::Format::checkedCount(data.virtualBytes.size()),
                   scheme.chartIo,
                   scheme.chartIoFill,
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
            .color = theme.scheme().chartCpu,
        });
    }
    memoryBars.push_back({.valueText = UI::Format::formatBytes(smoothed.virtualBytes),
                          .label = MEM_VIRTUAL_LABEL,
                          .tooltipText = {},
                          .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.virtualBytes, virtAxisUpper),
                          .color = theme.scheme().chartIo});

    return memoryBars;
}

/// The Resources chart's series: Threads and Handles/FDs on the count axis, Page Faults on the right
/// (#1024), and GDI Objects on Windows (possibly shorter, drawn from its offset, #1001).
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
        {.label = THREADS_LABEL, .color = theme.scheme().chartCpu, .value = App::Detail::formatCountOrNA(data.threads[*idx])},
        {.label = HANDLE_LABEL, .color = theme.scheme().chartMemory, .value = App::Detail::formatCountOrNA(data.handles[*idx])},
        {.label = FAULTS_LABEL,
         .color = theme.accentColor(3),
         .value = UI::Widgets::formatSampleOrNA(data.faults[*idx], [](double v) { return UI::Format::formatCountPerSecond(v); })},
    };
#ifdef _WIN32
    if (data.hasGdiSamples)
    {
        const auto gdiValue = App::Detail::seriesValueAt(data.gdi, data.gdiTimeOffset, *idx);
        rows.push_back({.label = GDI_LABEL,
                        .color = theme.accentColor(4),
                        .value = gdiValue ? UI::Format::formatIntLocalized(std::llround(*gdiValue)) : std::string("N/A")});
    }
#endif
    UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
}

/// The Resources chart's lines inside its plot, and the tooltip.
void drawResourceSeries(const ChartTimeAxis& axis, const ResourceChartData& data)
{
    const auto& theme = UI::Theme::get();
    const int plotCount = UI::Format::checkedCount(axis.alignedCount);
    plotSeries(THREADS_LABEL,
               axis.timeData.data(),
               data.threads.data(),
               plotCount,
               theme.scheme().chartCpu,
               theme.scheme().chartCpuFill,
               seriesStyle(SeriesRole::Primary));
    plotSeries(HANDLE_LABEL,
               axis.timeData.data(),
               data.handles.data(),
               plotCount,
               theme.scheme().chartMemory,
               theme.scheme().chartMemoryFill,
               seriesStyle(SeriesRole::Secondary, 0));
    ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
    plotSeries(FAULTS_LABEL,
               axis.timeData.data(),
               data.faults.data(),
               plotCount,
               theme.accentColor(3),
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
                   theme.accentColor(4),
                   std::nullopt,
                   seriesStyle(SeriesRole::Secondary, 2));
    }
#endif

    resourceTooltip(axis, data);
}

/// The Resources chart's NowBars: Threads and Handles/FDs on the count axis (@p countAxisUpper), Page
/// Faults on its own (@p faultAxisUpper), and GDI Objects on Windows -- four there, the
/// PROCESS_OVERVIEW_NOW_BAR_COLUMNS the Overview reserves.
[[nodiscard]] NowBarList resourceNowBars(const App::Detail::ProcessSmoothedUsage& smoothed, double countAxisUpper, double faultAxisUpper)
{
    const auto& theme = UI::Theme::get();
    const NowBar threadsBar{.valueText = UI::Format::formatIntLocalized(std::llround(smoothed.threadCount)),
                            .label = THREADS_LABEL,
                            .tooltipText = {}, // The fallback, "Threads: <value>", says it all (#1019)
                            .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.threadCount, countAxisUpper),
                            .color = theme.scheme().chartCpu};
    // An unreadable count (#1110) shows N/A, as its line shows a gap.
    const NowBar handlesBar{
        .valueText = App::Detail::countTextOrNA(smoothed.handleCountAvailable, smoothed.handleCount),
        .label = HANDLE_LABEL,
        .tooltipText = {}, // The fallback, "<label>: <value>", says it all (#1019)
        .value01 = smoothed.handleCountAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.handleCount, countAxisUpper) : 0.0,
        .color = theme.scheme().chartMemory};
    const NowBar faultsBar{.valueText = UI::Format::formatCountPerSecond(smoothed.pageFaultsPerSec),
                           .label = FAULTS_LABEL,
                           .tooltipText = {},
                           .value01 = UI::Widgets::normalizeToUnitInterval(smoothed.pageFaultsPerSec, faultAxisUpper),
                           // The line's colour: this bar was chartIo while its line is accentColor(3) (#1004).
                           .color = theme.accentColor(3)};
#ifdef _WIN32
    // GDI objects NowBar (Windows-only). A missing reading is NaN: it is skipped by the axis bound and
    // shown as N/A, and a series with no reading at all -- a process TaskSmack cannot open -- is not
    // drawn (#1000).
    const NowBar gdiBar{.valueText = App::Detail::countTextOrNA(smoothed.gdiInitialized, smoothed.gdiObjectCount),
                        .label = GDI_LABEL,
                        .tooltipText = {}, // The fallback, "GDI Objects: <value>", says it all (#1019)
                        .value01 =
                            smoothed.gdiInitialized ? UI::Widgets::normalizeToUnitInterval(smoothed.gdiObjectCount, countAxisUpper) : 0.0,
                        .color = theme.accentColor(4)};
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
    ImGui::Separator();
    // Only where the platform measures it: Windows does not, and used to chart a fabricated figure (#1028).
    if (ctx.hasPowerUsage)
    {
        renderPowerUsage(ctx, fill);
        ImGui::Separator();
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
                             .color = theme.scheme().chartCpu}; // The Total line's colour (#1192)
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
            // Total in its line's colour, not progressColor, which matched nothing on the chart.
            const std::array rows{
                UI::Widgets::TooltipRow{
                    .label = CPU_TOTAL_LABEL, .color = theme.scheme().chartCpu, .value = UI::Format::percentOneDecimal(cpuData[*idx])},
                UI::Widgets::TooltipRow{
                    .label = CPU_USER_LABEL, .color = theme.scheme().cpuUser, .value = UI::Format::percentOneDecimal(cpuUserData[*idx])},
                UI::Widgets::TooltipRow{.label = CPU_SYSTEM_LABEL,
                                        .color = theme.scheme().cpuSystem,
                                        .value = UI::Format::percentOneDecimal(cpuSystemData[*idx])},
            };
            UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  CPU (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessCPUHistoryOverview",
                             fill.plotHeight(),
                             cpuPlot,
                             {cpuTotalNow, cpuUserNow, cpuSystemNow},
                             false,
                             Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
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

    // These bands and lines are drawn with ImPlot directly, so they are capped here like every
    // plotLineWithFill series (#1022). Reduced together, so the bands still line up with each other
    // and with the lines. Points are chosen by each drawn value -- User (also the user band's top),
    // System and Total -- not by the cumulative system top, which stays flat when System rises as User
    // falls and would drop that spike.
    //
    // The choice of points is kept until the history changes (historyGeneration, #1139): reducing and
    // copying the whole history every frame was most of this chart's cost at the largest history
    // settings. Each frame only builds the kept points, at most LINE_PLOT_MAX_POINTS_DENSE of them,
    // into reused member buffers.
    const std::span<const UI::Widgets::ReducedPoint> points = m_CpuPlotReduction.points(
        {.generation = historyGeneration, .dataId = 0, .count = timeData.size(), .maxOut = UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE},
        [&](std::vector<UI::Widgets::ReducedPoint>& out)
        {
            UI::Widgets::reduceAlignedPoints<double>(
                timeData, {user, system, total}, UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE, nowSeconds, out);
        });

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
                      theme.scheme().chartCpu,
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

    // Shared is not reported on Windows; its line, tooltip row and bar are left out there rather than
    // shown as a permanent 0 (#1035).
    const bool showShared = ctx.hasSharedMemory;
    const MemoryChartData data{
        .used = tailSpan(history.series(ProcessSeries::MemoryUsed), alignedCount),
        .shared = showShared ? tailSpan(history.series(ProcessSeries::MemoryShared), alignedCount) : std::span<const double>{},
        .virtualBytes = tailSpan(history.series(ProcessSeries::Virtual), alignedCount),
        .peakBytes = ctx.peakMemoryBytes,
        .percentPerByte = smoothed.memoryPercentPerByte,
    };

    // Used and Shared in bytes on an axis that scales to them (#1195), eased like a rate axis and
    // shared with their bars.
    const double memAxisUpper = UI::Widgets::easedRateAxisUpperBound(
        "##ProcOverviewMemory",
        Detail::memoryAxisDataMax(axis.timeData, axis.axisConfig.xMin, data.used, data.shared, smoothed),
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
            UI::Widgets::setupSecondaryRateAxis(virtAxisUpper, UI::Widgets::formatAxisBytes, theme.scheme().chartIo);
            UI::Widgets::drawCollectingHint(alignedCount);
            drawMemorySeries(axis, data);
        }
    };

    ImGui::Spacing();
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory (%zu samples)", alignedCount);
    // Peak Memory is a line with a tooltip row but no bar; list it in the value strip too (#1193).
    const std::array peakEntry{UI::Widgets::ValueStripEntry{
        .label = MEM_PEAK_LABEL,
        .value = UI::Format::formatBytes(ctx.peakMemoryBytes),
        .color = theme.scheme().chartPeakLine,
    }};
    const std::span<const UI::Widgets::ValueStripEntry> stripExtras = (ctx.peakMemoryBytes > 0.0)
                                                                        ? std::span<const UI::Widgets::ValueStripEntry>(peakEntry)
                                                                        : std::span<const UI::Widgets::ValueStripEntry>{};
    renderHistoryWithNowBars("ProcessMemoryOverviewLayout",
                             fill.plotHeight(),
                             memoryPlot,
                             memoryBars,
                             false,
                             Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS,
                             false,
                             UI::Widgets::NowBarValues::Strip,
                             stripExtras);
    fill.addPlot();
    ImGui::Spacing();
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

    ResourceChartData data{
        .threads = tailSpan(history.series(ProcessSeries::Threads), alignedCount),
        .handles = tailSpan(history.series(ProcessSeries::Handles), alignedCount),
        .faults = tailSpan(history.series(ProcessSeries::PageFaults), alignedCount),
    };
#ifdef _WIN32
    // The GDI history can be shorter than the others, and ends at the same newest sample, so it starts
    // gdiTimeOffset timestamps in (#1001).
    const std::size_t gdiAlignedCount = std::min(alignedCount, history.series(ProcessSeries::GdiObjects).size());
    data.gdi = tailSpan(history.series(ProcessSeries::GdiObjects), gdiAlignedCount);
    data.gdiTimeOffset = Detail::seriesTimeOffset(alignedCount, data.gdi.size());
    data.hasGdiSamples = Detail::hasAnySample(data.gdi);
    const double countSeriesMax = Detail::countAxisDataMaxWithGdi(axis.timeData, xMin, data.threads, data.handles, data.gdi, smoothed);
#else
    const double countSeriesMax = Detail::countAxisDataMax(axis.timeData, xMin, data.threads, data.handles, smoothed);
#endif

    // Threads, handles (and GDI objects) are counts on the left axis; page faults are a rate, on their
    // own right-hand axis, so a fault spike no longer flattens the count lines (#1024). Each bound
    // covers every series drawn on its axis and its bars' smoothed values (#1003, #1145).
    const double countAxisUpper =
        UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults", countSeriesMax, UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);
    const double faultAxisUpper = UI::Widgets::easedRateAxisUpperBound("##ProcThreadsFaults/Y2",
                                                                       Detail::faultAxisDataMax(axis.timeData, xMin, data.faults, smoothed),
                                                                       UI::Widgets::RATE_AXIS_MIN_SPAN_COUNT);

    const NowBarList bars = resourceNowBars(smoothed, countAxisUpper, faultAxisUpper);

    auto plot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
            UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                        "##ProcThreadsFaults", xMin, axis.axisConfig.xMax, formatAxisLocalized, countAxisUpper),
                                    fill.plotHeight()),
            ctx.historyGeneration));
        if (chart.active())
        {
            UI::Widgets::setupSecondaryRateAxis(faultAxisUpper, formatAxisLocalized, theme.accentColor(3));
            UI::Widgets::drawCollectingHint(alignedCount);
            drawResourceSeries(axis, data);
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_GEARS "  Resources (%zu samples)", alignedCount);
    renderHistoryWithNowBars("ProcessResourceHistory", fill.plotHeight(), plot, bars, false, Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
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
                          .color = theme.scheme().textInfo};

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
        plotLineWithFill(
            POWER_LABEL, axis.timeData.data(), powerData.data(), UI::Format::checkedCount(powerData.size()), theme.scheme().textInfo);
        if (const auto idx = hoveredSampleIndex(axis.timeData, powerData.size()))
        {
            const std::array rows{UI::Widgets::TooltipRow{
                .label = POWER_LABEL,
                .color = theme.scheme().textInfo,
                .value = UI::Widgets::formatSampleOrNA(powerData[*idx], [](double v) { return UI::Format::formatPowerOrZero(v); })}};
            UI::Widgets::renderHistoryTooltip(axis.timeData[*idx], rows);
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_BOLT "  Power Usage (%zu samples)", axis.alignedCount);
    renderHistoryWithNowBars("ProcessPowerHistory", fill.plotHeight(), plot, {powerBar}, false, Detail::PROCESS_OVERVIEW_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
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
    ImGui::Separator();
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
                         .color = theme.scheme().chartIo};
    const NowBar writeBar{.valueText = Detail::rateTextOrNA(ioAvailable, smoothed.ioWriteBytesPerSec),
                          .label = IO_WRITE_LABEL,
                          .tooltipText = {},
                          .value01 = ioAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.ioWriteBytesPerSec, ioAxisUpper) : 0.0,
                          .color = theme.scheme().chartIoWrite};

    // Keep the plot and its hover tooltip together: both consume the same aligned vectors, and the
    // lambda is rendered alongside the matching NowBars below.
    auto plot = [&]()
    {
        drawTwoRatePlot(
            "##ProcIoHistory",
            axis,
            ioAxisUpper,
            fill.plotHeight(),
            ctx.historyGeneration,
            {.label = IO_READ_LABEL, .data = readData, .color = theme.scheme().chartIo, .fill = theme.scheme().chartIoFill},
            {.label = IO_WRITE_LABEL, .data = writeData, .color = theme.scheme().chartIoWrite, .fill = theme.scheme().chartIoWriteFill});
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  I/O Statistics (%zu samples)", axis.alignedCount);
    renderHistoryWithNowBars(
        "ProcessIoHistory", fill.plotHeight(), plot, {readBar, writeBar}, false, Detail::PROCESS_NETWORK_IO_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
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
                         .color = theme.scheme().chartNetTx};
    const NowBar recvBar{.valueText = Detail::rateTextOrNA(netAvailable, smoothed.netRecvBytesPerSec),
                         .label = NET_RECV_LABEL,
                         .tooltipText = {},
                         .value01 = netAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.netRecvBytesPerSec, netAxisUpper) : 0.0,
                         .color = theme.scheme().chartNetRx};

    // The plot lambda owns rendering and hover lookup over the same aligned buffers;
    // renderHistoryWithNowBars composes it with the summary bars. The tooltip's rates are averages
    // since the process was first seen, as the chart's heading says.
    auto plot = [&]()
    {
        drawTwoRatePlot(
            "##ProcNetworkHistory",
            axis,
            netAxisUpper,
            fill.plotHeight(),
            ctx.historyGeneration,
            {.label = NET_SENT_LABEL, .data = sentData, .color = theme.scheme().chartNetTx, .fill = theme.scheme().chartNetTxFill},
            {.label = NET_RECV_LABEL, .data = recvData, .color = theme.scheme().chartNetRx, .fill = theme.scheme().chartNetRxFill});
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_NETWORK_WIRED "  Network (%zu samples)", axis.alignedCount);
    // The heading's tooltip is shown after the chart: its value strip is placed beside the heading,
    // the item drawn just before it, so nothing else is submitted between the two.
    const bool headingHovered = ImGui::IsItemHovered();
    renderHistoryWithNowBars(
        "ProcessNetworkHistory", fill.plotHeight(), plot, {sentBar, recvBar}, false, Detail::PROCESS_NETWORK_IO_NOW_BAR_COLUMNS);
    if (headingHovered)
    {
        ImGui::SetTooltip("Network bytes/sec between readings of the process's open connections. A refresh that reuses a cached reading "
                          "shows the last rate.");
    }
    fill.addPlot();
    ImGui::Spacing();
}

void ProcessDetailsCharts::renderGpuTab(const ProcessChartContext& ctx)
{
    if (ctx.history == nullptr || ctx.smoothed == nullptr || ctx.snapshot == nullptr)
    {
        return;
    }
    // A failed read is not "not available on this system" (#1210).
    const Detail::GpuTabContent gpuContent = Detail::gpuTabContentFor(*ctx.snapshot, *ctx.history, ctx.rateReadings.gpuSupported);
    if (gpuContent == Detail::GpuTabContent::Unavailable)
    {
        // Not "no usage": without per-process metrics none can be seen (#1210).
        UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  Per-process GPU usage is not available",
                                      "This system's GPU monitoring does not report GPU usage per process.");
    }
    else if (gpuContent == Detail::GpuTabContent::NoReadings)
    {
        // Every retained read failed: nothing is known yet about this process's GPU use (#1210).
        UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  No GPU readings yet", "Reading this process's GPU usage has not succeeded yet.");
    }
    else if (gpuContent == Detail::GpuTabContent::NoUsage)
    {
        // Only the retained history is looked at, so the text names that window (#1210); rebuilt only
        // when the window changes.
        if (m_NoGpuUsageDetail.empty() || m_NoGpuUsageDetailSeconds != ctx.maxHistorySeconds)
        {
            m_NoGpuUsageDetail = Detail::noGpuUsageDetail(ctx.maxHistorySeconds);
            m_NoGpuUsageDetailSeconds = ctx.maxHistorySeconds;
        }
        UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  No GPU usage", m_NoGpuUsageDetail.c_str());
    }
    else
    {
        // The two history charts share the tab's height, like the other tabs' charts (#959). The
        // metrics table and per-GPU breakdown above them count as non-plot.
        UI::Widgets::FillPlotLayout fill(m_GpuFill);
        const UI::Widgets::AlignedChartStack alignedCharts("##ProcGpuCharts"); // #1206
        renderGpuUsage(ctx, fill);
    }
}

void ProcessDetailsCharts::renderGpuUsage(const ProcessChartContext& ctx, UI::Widgets::FillPlotLayout& fill)
{
    const auto& proc = *ctx.snapshot;
    const auto& theme = UI::Theme::get();

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

    // Show GPU info
    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_MICROCHIP "  GPU Usage");
    ImGui::Spacing();

    renderGpuCurrentMetricsTable(ctx);

    ImGui::Spacing();
    // With one GPU the breakdown repeats the table above (#1207).
    if (Detail::shouldShowPerGpuBreakdown(proc.perGpuUsage.size()))
    {
        ImGui::Separator();
        ImGui::Spacing();

        renderPerGpuBreakdown(ctx);
    }

    ImGui::Separator();
    ImGui::Spacing();

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

    // One label/value row; the value in @p color, or the default text colour without one.
    const auto row = [](const char* label, const std::string& value, const ImVec4* color)
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
    };

    // Current GPU metrics
    if (!ImGui::BeginTable("GPUCurrentMetrics", 2, ImGuiTableFlags_SizingStretchProp))
    {
        return;
    }
    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, UI::Widgets::measureLabelColumnWidth(LABELS));
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

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

    // One label/value row; the value in @p color, or the default text colour without one.
    const auto row = [](const char* label, const std::string& value, const ImVec4* color)
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
    };

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
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

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
    const auto gpuUtilVec = tailSpan(history.series(ProcessSeries::GpuUtil), axis.alignedCount);
    const auto gpuMemVec = tailSpan(history.series(ProcessSeries::GpuMemory), axis.alignedCount);

    // GPU Utilization graph (percent metric: locked 0-100 axis with percent formatter)
    auto plotGpuUtil = [&]()
    {
        drawGpuHistoryPlot(
            UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig("##GPUUtilPlot", axis.axisConfig.xMin, axis.axisConfig.xMax),
                                        fill.plotHeight()),
                ctx.historyGeneration),
            axis,
            gpuUtilVec,
            GPU_UTIL_LABEL,
            GPU_UTIL_LABEL,
            theme.scheme().gpuUtilization,
            theme.scheme().gpuUtilizationFill,
            [](double v) { return UI::Format::percentOneDecimal(v); });
    };

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
            [](double v) { return UI::Widgets::formatSampleOrNA(v, [](double b) { return UI::Format::formatBytes(b); }); });
    };

    // Now bars for current values
    const NowBar gpuUtilBar{
        .valueText = Detail::gpuUtilizationText(smoothed.gpuUtilAvailable, smoothed.gpuUtilPercent),
        .label = GPU_UTIL_LABEL,
        .tooltipText = {},
        .value01 = smoothed.gpuUtilAvailable ? UI::Format::percent01(smoothed.gpuUtilPercent) : 0.0,
        .color = theme.scheme().gpuUtilization,
    };
    const NowBar gpuMemBar{
        .valueText = smoothed.gpuMemoryAvailable ? UI::Format::formatBytes(smoothed.gpuMemoryBytes) : std::string("N/A"),
        .label = GPU_MEMORY_LABEL,
        .tooltipText = {},
        .value01 = smoothed.gpuMemoryAvailable ? UI::Widgets::normalizeToUnitInterval(smoothed.gpuMemoryBytes, gpuMemAxisUpper) : 0.0,
        .color = theme.scheme().gpuMemory,
    };

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Utilization History (%zu samples)", axis.alignedCount);
    renderHistoryWithNowBars(
        "ProcessGPUUtilHistory", fill.plotHeight(), plotGpuUtil, {gpuUtilBar}, false, Detail::PROCESS_GPU_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();

    ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_CHART_LINE "  GPU Memory History (%zu samples)", axis.alignedCount);
    renderHistoryWithNowBars(
        "ProcessGPUMemHistory", fill.plotHeight(), plotGpuMem, {gpuMemBar}, false, Detail::PROCESS_GPU_NOW_BAR_COLUMNS);
    fill.addPlot();
    ImGui::Spacing();
}

} // namespace App
