#include "StorageSection.h"

#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "UI/ChartGrid.h"
#include "UI/ChartGridLayout.h"
#include "UI/ChartWidgets.h"
#include "UI/EmptyState.h"
#include "UI/Format.h"
#include "UI/HistoryPlotHeight.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/InlineText.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace App::StorageSection
{

namespace
{

using UI::Widgets::ChartGridConfig;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::normalizeToUnitInterval;
using UI::Widgets::NowBar;
using UI::Widgets::NowBarValues;
using UI::Widgets::plotSeries;
using UI::Widgets::renderChartGrid;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;
using UI::Widgets::sharedAxisUpperBound;
using UI::Widgets::tailAlignedSpan;

/// One disk cell's data for a frame, gathered before the grid draws (#1299).
struct DiskCellFrame
{
    std::span<const double> times;
    std::span<const double> readData;
    std::span<const double> writeData;
    double currentRead = std::numeric_limits<double>::quiet_NaN();
    double currentWrite = std::numeric_limits<double>::quiet_NaN();
};

constexpr size_t STORAGE_NOW_BAR_COLUMNS = 2; // Read, Write

// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008).
constexpr const char* READ_LABEL = "Read";
constexpr const char* WRITE_LABEL = "Write";

// Narrowest a disk cell may get before the grid uses fewer columns instead, in ems.
constexpr float MIN_DISK_CELL_WIDTH_EM = 30.0F;

/// Minimum plot height a disk cell will shrink to before the grid prefers scrolling over squashing
/// charts flat. The same font-relative floor the Overview's charts hold (UI/HistoryPlotHeight.h),
/// in whole pixels, so both bounds of the shared height rule apply here and not only the ceiling.
/// It was a fixed 60px, which at Even Huger is under three lines of axis text.
[[nodiscard]] float minDiskPlotHeight()
{
    return std::floor(UI::Widgets::historyPlotMinHeight(ImGui::GetFontSize(), UI::chartEmPx()));
}

/// The per-disk grid's sizing for a region @p availableWidth x @p availableHeight, shared by the
/// grid itself and diskGridMinimumHeight() so the reserve the Network and I/O tab keeps for the grid
/// is the grid it then gets.
[[nodiscard]] ChartGridConfig diskGridConfig(float availableWidth, float availableHeight, std::size_t diskCount)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    // Approximate overhead used only as a floor for the grid's minimum cell height; the real
    // per-cell overhead is measured directly in renderDiskCell via cursor position (see its
    // doc comment and the #823 review that replaced an earlier hand-guessed constant here).
    // Includes every fixed cost between the grid's chosen outer cellHeight and the plot it
    // wraps: the bordered GridCell child's own WindowPadding (ChartGrid.h reserves it before
    // renderDiskCell ever sees a height), the label row, and renderDiskCell's nested table
    // CellPadding -- omitting any of those understates the floor, so the grid can pick a
    // cellHeight that only fits a plot smaller than minDiskPlotHeight() once the real overhead is
    // subtracted, which then clips invisibly against the cell's NoScrollbar instead of the
    // grid falling back to more rows/scrolling (#823 review).
    // Two text lines: the disk's name and, under it, its Read/Write value strip (#1193).
    const float approxLabelOverhead =
        (style.WindowPadding.y * 2.0F) + (ImGui::GetTextLineHeight() * 2.0F) + (style.ItemSpacing.y * 2.0F) + (style.CellPadding.y * 2.0F);
    return ChartGridConfig{
        .availableWidth = availableWidth,
        .availableHeight = availableHeight,
        .itemCount = diskCount,
        // 30 em is the former fixed 320px at the reference em (32/3 px): the width floor now
        // scales with the font like the height floor beside it, so a large font gets fewer,
        // wider cells rather than rate labels and NowBars crowding a fixed 320px (#964).
        .minCellWidth = MIN_DISK_CELL_WIDTH_EM * ImGui::GetFontSize(),
        // The same floor and ceiling the Overview's charts keep to (UI/HistoryPlotHeight.h), so the
        // two tabs follow one rule instead of one never growing and the other never stopping (#923).
        .minCellHeight = approxLabelOverhead + minDiskPlotHeight(),
        .maxCellHeight = approxLabelOverhead + UI::Widgets::historyPlotMaxHeight(ImGui::GetFontSize()),
        .targetCellAspect = 1.0F,
        // What renderChartGrid() sets around every cell (its table's CellPadding).
        .columnOverhead = style.CellPadding.x * 2.0F,
        .rowOverhead = style.CellPadding.y * 2.0F,
    };
}

/// Render a single disk cell (label + read/write NowBars + chart). cellHeight is the enclosing
/// grid cell's *usable content* height (see renderChartGrid's cellWidth/cellHeight doc in
/// ChartGrid.h -- it's measured via GetContentRegionAvail() inside the cell's BeginChild, not
/// the outer size passed to it); the plot height is derived from it by measuring the label row's
/// actual consumed height via cursor position (rather than guessing at ImGui's spacing rules
/// with a hand-picked constant -- see #823 review) so the chart fills exactly what's left in the
/// cell.
///
/// The overhead is measured on the first disk and reused for the rest, and cached across frames too
/// until the style metrics it's built from change (UI::Widgets::CellOverheadCache): every cell gets
/// the same cellHeight (ImGuiTableFlags_SizingStretchSame) and renders an identically-shaped label
/// row, so the resulting vertical overhead is the same across all disks and doesn't change frame to
/// frame on its own.
///
/// diskAxisUpper is the grid's shared Y upper bound (sharedAxisUpperBound(), #1299), for the chart's
/// axis and its bars alike, so a bar and its line show a value at the same height (#1003).
void renderDiskCell(const std::string& deviceName,
                    std::span<const double> timeData,
                    std::span<const double> readData,
                    std::span<const double> writeData,
                    double currentRead,
                    double currentWrite,
                    double diskAxisUpper,
                    const UI::Widgets::TimeAxisConfig& axisConfig,
                    const UI::Theme& theme,
                    float cellHeight,
                    UI::Widgets::CellOverheadCache& overheadCache,
                    const UI::Widgets::CellStyleMetrics& styleMetrics,
                    std::uint64_t dataGeneration)
{
    // The cell has no legend, so its value strip is the chart's key: each bar carries its series'
    // marker, so Read (the filled primary) and Write (a secondary's marker) differ by more than colour.
    const auto makeBar = [&](const char* label, double current, const ImVec4& color, ImPlotMarker marker)
    {
        if (!std::isfinite(current))
        {
            return NowBar{.valueText = "N/A",
                          .label = label,
                          .tooltipText = UI::InlineText::format("{}: not reported this sample", label),
                          .value01 = 0.0,
                          .color = theme.scheme().textMuted,
                          .marker = marker};
        }
        return NowBar{.valueText = UI::Format::formatBytesPerSec(current),
                      .label = label,
                      .tooltipText = {},
                      .value01 = normalizeToUnitInterval(current, diskAxisUpper),
                      .color = color,
                      .marker = marker};
    };
    const std::array diskBars{
        makeBar(READ_LABEL, currentRead, theme.scheme().chartIo, seriesStyle(SeriesRole::Primary).marker),
        makeBar(WRITE_LABEL, currentWrite, theme.scheme().chartIoWrite, seriesStyle(SeriesRole::Secondary, 0).marker),
    };

    const float cellContentTop = ImGui::GetCursorPosY();
    ImGui::TextColored(theme.scheme().textPrimary, "%.*s", static_cast<int>(deviceName.size()), deviceName.data());
    // The disk's current rates, readable without hovering (#1193), on their own line under its name.
    // Compact: exactly one line of "Read: 1.2 MiB/s" / "Read: N/A", never wrapped -- the overhead below
    // is measured once and the grid budgets one strip line per cell, and the longer "not reported
    // this sample" tooltip text would not fit a minimum-width cell. Hovering a bar still shows it.
    UI::Widgets::renderNowBarValueStrip(diskBars, {}, UI::Widgets::ValueStripLayout::Compact);
    float measuredOverhead = 0.0F;
    if (const auto cached = overheadCache.get(styleMetrics))
    {
        measuredOverhead = *cached;
    }
    else
    {
        // renderHistoryWithNowBars wraps the chart+bars in its own table, whose CellPadding.y
        // (top+bottom) adds a little more height beyond the label -- account for it here rather
        // than clipping the chart against it (#823 review: residual scrollbar after the cell's own
        // WindowPadding was already corrected for).
        measuredOverhead = (ImGui::GetCursorPosY() - cellContentTop) + (ImGui::GetStyle().CellPadding.y * 2.0F);
        overheadCache.store(styleMetrics, measuredOverhead);
    }
    const float plotHeight = std::max(minDiskPlotHeight(), cellHeight - measuredOverhead);

    auto diskPlotFn = [&]()
    {
        // deviceName.c_str() (not a constant "##DiskPlot"), so RenderMetrics records a distinct
        // entry per disk instead of collapsing every disk's plot into one: HistoryChart reads
        // config.id verbatim as its RenderMetrics key, ignoring the surrounding PushID(deviceName)
        // scope entirely -- that scope only disambiguates ImGui/ImPlot's own widget state, not
        // this. ImPlotFlags_NoTitle keeps the plot title hidden (deviceName has no "##" prefix to
        // hide it via ImPlot's usual Label##ID convention) without needing to allocate a new
        // string just to add one (#823 review).
        auto diskCfg = UI::Widgets::rateHistoryConfigWithUpper(
            deviceName.c_str(), axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec, diskAxisUpper);
        diskCfg.flags |= ImPlotFlags_NoTitle;
        // No "Time (s)" or time tick labels in each of the cells (#1206); like every chart, no legend
        // (#1198): the cell's value strip above names Read and Write.
        diskCfg.timeAxisLabels = false;
        diskCfg.height = plotHeight;
        const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(diskCfg, dataGeneration));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(timeData.size()); // The same "no data yet" state on every chart (#1013)
            const int count = UI::Format::checkedCount(timeData.size());
            plotSeries(READ_LABEL,
                       timeData.data(),
                       readData.data(),
                       count,
                       theme.scheme().chartIo,
                       theme.scheme().chartIoFill,
                       seriesStyle(SeriesRole::Primary));
            plotSeries(WRITE_LABEL,
                       timeData.data(),
                       writeData.data(),
                       count,
                       theme.scheme().chartIoWrite,
                       theme.scheme().chartIoWriteFill,
                       seriesStyle(SeriesRole::Secondary, 0));

            if (ImPlot::IsPlotHovered() && !timeData.empty())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < timeData.size())
                    {
                        const std::array rows{
                            UI::Widgets::TooltipRow{.label = READ_LABEL,
                                                    .color = theme.scheme().chartIo,
                                                    .value = UI::Format::formatBytesPerSecOrNA(static_cast<double>(readData[*idxVal]))},
                            UI::Widgets::TooltipRow{.label = WRITE_LABEL,
                                                    .color = theme.scheme().chartIoWrite,
                                                    .value = UI::Format::formatBytesPerSecOrNA(static_cast<double>(writeData[*idxVal]))},
                        };
                        UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                    }
                }
            }
        }
    };

    // deviceName is a real std::string (not string_view), so .c_str() is a guaranteed
    // null-terminated C-string at zero extra cost -- reusing it as the RenderMetrics/table id
    // keeps per-disk RenderMetrics entries from collapsing into one (#823 review), without
    // reintroducing the per-frame heap allocation a formatted id had.
    renderHistoryWithNowBars(
        deviceName.c_str(), plotHeight, diskPlotFn, diskBars, false, STORAGE_NOW_BAR_COLUMNS, false, NowBarValues::None);
}

} // namespace

float diskGridMinimumHeight(const Domain::StoragePublication* publication, float availableWidth)
{
    if (!usesDiskGrid(publication))
    {
        return 0.0F;
    }
    // The grid's heading line, then its rows (renderStorageSection()).
    return ImGui::GetTextLineHeightWithSpacing() +
           UI::Widgets::computeChartGridMinimumHeight(diskGridConfig(availableWidth, 0.0F, publication->perDiskHistory.size()));
}

void updateSmoothedDiskIO(double targetRead, double targetWrite, float deltaTimeSeconds, RenderContext& ctx)
{
    if (ctx.smoothedReadBytesPerSec == nullptr || ctx.smoothedWriteBytesPerSec == nullptr || ctx.smoothedInitialized == nullptr)
    {
        return;
    }

    const double alpha = computeAlpha(deltaTimeSeconds, ctx.refreshInterval);

    const bool initialized = *ctx.smoothedInitialized;
    *ctx.smoothedReadBytesPerSec = initializeOrSmooth(*ctx.smoothedReadBytesPerSec, targetRead, alpha, initialized);
    *ctx.smoothedWriteBytesPerSec = initializeOrSmooth(*ctx.smoothedWriteBytesPerSec, targetWrite, alpha, initialized);
    *ctx.smoothedInitialized = true;
}

void renderStorageSection(RenderContext& ctx)
{
    const auto& theme = UI::Theme::get();
    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    if (ctx.publication == nullptr)
    {
        UI::Widgets::renderEmptyState(ICON_FA_HARD_DRIVE "  Disk data unavailable",
                                      "The storage model is not available, so there is no disk activity to show.");
        return;
    }

    const auto& diskSnap = ctx.publication->snapshot;
    const auto& diskTimestamps = ctx.publication->timestamps;
    const size_t historySize = diskTimestamps.size();

    const auto diskAxis = historySize > 0 ? makeTimeAxisConfig(diskTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                          : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Build shared time axis (float, relative)
    std::span<const double> diskTimes;
    if (historySize > 0)
    {
        diskTimes = frameTimeAxis(diskTimestamps, historySize, nowSeconds);
    }

    // Update aggregate smoothed values
    updateSmoothedDiskIO(diskSnap.totalReadBytesPerSec, diskSnap.totalWriteBytesPerSec, ctx.lastDeltaSeconds, ctx);

    const double smoothedRead = ctx.smoothedReadBytesPerSec != nullptr ? *ctx.smoothedReadBytesPerSec : diskSnap.totalReadBytesPerSec;
    const double smoothedWrite = ctx.smoothedWriteBytesPerSec != nullptr ? *ctx.smoothedWriteBytesPerSec : diskSnap.totalWriteBytesPerSec;

    const auto& perDisk = ctx.publication->perDiskHistory;
    const size_t diskCount = perDisk.size();

    if (usesDiskGrid(ctx.publication))
    {
        // ── Multi-disk: one chart cell per disk, grid fills the available panel space ──
        ImGui::TextColored(
            theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  Disk I/O by Device (%zu disks, %zu samples)", diskCount, historySize);

        const double diskAlpha = computeAlpha(ctx.lastDeltaSeconds, ctx.refreshInterval);
        if (ctx.smoothedPerDisk != nullptr)
        {
            // Forget disks no longer listed (unplugged and pruned from the history), so the map
            // stays the size of the grid.
            std::erase_if(*ctx.smoothedPerDisk,
                          [&](const auto& entry)
                          { return std::ranges::none_of(perDisk, [&](const auto& disk) { return disk.deviceName == entry.first; }); });
        }

        // Measured once (by renderDiskCell, on the first disk) and reused for the rest, and across
        // frames until the style metrics it's built from change -- see renderDiskCell's doc comment.
        static UI::Widgets::CellOverheadCache overheadCache;
        const UI::Widgets::CellStyleMetrics styleMetrics{
            .textLineHeight = ImGui::GetTextLineHeight(),
            .itemSpacingY = ImGui::GetStyle().ItemSpacing.y,
            .cellPaddingY = ImGui::GetStyle().CellPadding.y,
        };

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        // The sizing diskGridMinimumHeight() reserved for this grid on the Network and I/O tab.
        const ChartGridConfig gridConfig = diskGridConfig(avail.x, avail.y, diskCount);

        // One frame's data for each disk cell, gathered before the grid draws so every cell can be
        // drawn to one shared Y bound (#1299). Reused across frames (UI thread only), so a frame
        // allocates nothing once it has seen as many disks (#1171).
        static std::vector<DiskCellFrame> diskFrames;
        static std::vector<double> diskUpperBounds;
        diskFrames.clear();
        diskUpperBounds.clear();
        for (size_t diskIdx = 0; diskIdx < diskCount; ++diskIdx)
        {
            const auto& disk = perDisk[diskIdx];
            const size_t alignedCount = std::min({diskTimes.size(), disk.readBytesPerSec.size(), disk.writeBytesPerSec.size()});

            // An empty history still draws the cell's chart, with the collecting hint, rather than
            // plain text in place of the chart (#1013).

            // Views into the published history, plotted as doubles against the double time axis --
            // no per-frame float copies (#1018). The pooled axis, viewed in place: no per-disk copy
            // (#1066 review).
            DiskCellFrame frame{
                .times = tailAlignedSpan(diskTimes, alignedCount).values,
                .readData = tailAlignedSpan(disk.readBytesPerSec, alignedCount).values,
                .writeData = tailAlignedSpan(disk.writeBytesPerSec, alignedCount).values,
            };

            // Per-disk snapshot values for NowBars. NaN if the disk is missing from the latest sample:
            // renderDiskCell shows N/A, not 0. The latest sample normally lists the disks in the
            // history's order, so the same index is checked first; a scan of its few disks covers a
            // disk added or removed since. A name -> snapshot map rebuilt every frame cost a heap
            // allocation per disk per frame for keys copied from strings already there (#1171).
            const Domain::DiskSnapshot* latestDisk = nullptr;
            if (diskIdx < diskSnap.disks.size() && diskSnap.disks[diskIdx].deviceName == disk.deviceName)
            {
                latestDisk = &diskSnap.disks[diskIdx];
            }
            else if (const auto it = std::ranges::find(diskSnap.disks, disk.deviceName, &Domain::DiskSnapshot::deviceName);
                     it != diskSnap.disks.end())
            {
                latestDisk = &*it;
            }
            if (latestDisk != nullptr)
            {
                frame.currentRead = latestDisk->readBytesPerSec;
                frame.currentWrite = latestDisk->writeBytesPerSec;
            }
            if (ctx.smoothedPerDisk != nullptr)
            {
                auto& smoothed = (*ctx.smoothedPerDisk)[disk.deviceName];
                if (std::isfinite(frame.currentRead) && std::isfinite(frame.currentWrite))
                {
                    smoothed.readBytesPerSec =
                        initializeOrSmooth(smoothed.readBytesPerSec, frame.currentRead, diskAlpha, smoothed.initialized);
                    smoothed.writeBytesPerSec =
                        initializeOrSmooth(smoothed.writeBytesPerSec, frame.currentWrite, diskAlpha, smoothed.initialized);
                    smoothed.initialized = true;
                    frame.currentRead = smoothed.readBytesPerSec;
                    frame.currentWrite = smoothed.writeBytesPerSec;
                }
                else
                {
                    // Absent this sample: the bars show N/A, and start afresh when the disk returns.
                    smoothed.initialized = false;
                }
            }

            // This disk's own eased bound (#1011), from the samples in the window (#1145) and its bars'
            // current values (#1003): a bar easing down from a peak that has just left the window is
            // not clamped. A per-disk series holds NaN for samples where the disk was absent, and the
            // current values are NaN when it is absent from the latest sample (#1015): both are skipped.
            // Keyed by device name, like the cell, so a disk keeps its easing when another is unplugged.
            ImGui::PushID(disk.deviceName.data(), disk.deviceName.data() + disk.deviceName.size());
            diskUpperBounds.push_back(UI::Widgets::easedRateAxisUpperBound(
                "##DiskAxis",
                UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(frame.times, diskAxis.xMin, frame.readData, frame.writeData),
                                               {frame.currentRead, frame.currentWrite}),
                UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC));
            ImGui::PopID();
            diskFrames.push_back(frame);
        }
        // Every cell is drawn to the largest disk's bound, so cells side by side compare at a glance
        // and an idle disk's noise is not scaled up to fill its cell (#1299).
        const double sharedDiskAxisUpper = sharedAxisUpperBound(diskUpperBounds, UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

        renderChartGrid(
            "PerDiskGrid",
            diskCount,
            gridConfig,
            [&](const size_t diskIdx, float /*cellWidth*/, const float cellHeight)
            {
                const DiskCellFrame& frame = diskFrames[diskIdx];
                renderDiskCell(perDisk[diskIdx].deviceName,
                               frame.times,
                               frame.readData,
                               frame.writeData,
                               frame.currentRead,
                               frame.currentWrite,
                               sharedDiskAxisUpper,
                               diskAxis,
                               theme,
                               cellHeight,
                               overheadCache,
                               styleMetrics,
                               ctx.chartDataGeneration);
            },
            // Disks can be unplugged mid-session, shifting later indices in perDisk -- key each
            // cell's ImGui/ImPlot state by the stable device name instead of position (#823 review).
            [&](const size_t diskIdx) -> std::string_view { return perDisk[diskIdx].deviceName; });
    }
    else
    {
        // ── Single disk (or no data yet): aggregate chart ─────────────────────
        const auto& diskReadHist = ctx.publication->totalReadHistory;
        const auto& diskWriteHist = ctx.publication->totalWriteHistory;
        const size_t alignedDisk = std::min({historySize, diskReadHist.size(), diskWriteHist.size()});

        // Take the newest alignedDisk entries of each series, so read, write and time line up by sample.
        const auto aggregateTimes = frameTimeAxis(diskTimestamps, alignedDisk, nowSeconds);
        // Views into the published history, plotted as doubles -- no per-frame float copies (#1018).
        const auto readData = tailAlignedSpan(diskReadHist, alignedDisk).values;
        const auto writeData = tailAlignedSpan(diskWriteHist, alignedDisk).values;

        // One upper bound for the chart's Y axis and its bars (#1003), from the samples in the window
        // (#1145) and the bars' smoothed values, which can lag a peak that has just left it.
        const double diskAxisUpper = UI::Widgets::easedRateAxisUpperBound(
            "##SystemDiskHistory",
            UI::Widgets::withCurrentValues(UI::Widgets::maxOfSeriesSince(aggregateTimes, diskAxis.xMin, readData, writeData),
                                           {smoothedRead, smoothedWrite}),
            UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);

        const NowBar readBar{.valueText = UI::Format::formatBytesPerSec(smoothedRead),
                             .label = READ_LABEL,
                             .tooltipText = {},
                             .value01 = normalizeToUnitInterval(smoothedRead, diskAxisUpper),
                             .color = theme.scheme().chartIo};
        const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(smoothedWrite),
                              .label = WRITE_LABEL,
                              .tooltipText = {},
                              .value01 = normalizeToUnitInterval(smoothedWrite, diskAxisUpper),
                              .color = theme.scheme().chartIoWrite};

        // Shares the tab's height with the network chart above it (#959).
        const float plotHeight = (ctx.fill != nullptr) ? ctx.fill->plotHeight() : HISTORY_PLOT_HEIGHT_DEFAULT;
        auto diskPlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::rateHistoryConfigWithUpper(
                                            "##SystemDiskHistory", diskAxis.xMin, diskAxis.xMax, formatAxisBytesPerSec, diskAxisUpper),
                                        plotHeight),
                ctx.chartDataGeneration));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedDisk); // The same "no data yet" state on every chart (#1013)
                const int count = UI::Format::checkedCount(alignedDisk);
                plotSeries(READ_LABEL,
                           aggregateTimes.data(),
                           readData.data(),
                           count,
                           theme.scheme().chartIo,
                           theme.scheme().chartIoFill,
                           seriesStyle(SeriesRole::Primary));
                plotSeries(WRITE_LABEL,
                           aggregateTimes.data(),
                           writeData.data(),
                           count,
                           theme.scheme().chartIoWrite,
                           theme.scheme().chartIoWriteFill,
                           seriesStyle(SeriesRole::Secondary, 0));

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(aggregateTimes, mouse.x))
                    {
                        if (*idxVal < alignedDisk)
                        {
                            const std::array rows{
                                UI::Widgets::TooltipRow{.label = READ_LABEL,
                                                        .color = theme.scheme().chartIo,
                                                        .value = UI::Format::formatBytesPerSecOrNA(static_cast<double>(readData[*idxVal]))},
                                UI::Widgets::TooltipRow{.label = WRITE_LABEL,
                                                        .color = theme.scheme().chartIoWrite,
                                                        .value =
                                                            UI::Format::formatBytesPerSecOrNA(static_cast<double>(writeData[*idxVal]))},
                            };
                            UI::Widgets::renderHistoryTooltip(aggregateTimes[*idxVal], rows);
                        }
                    }
                }
            }
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  Disk I/O History (%zu samples)", alignedDisk);
        renderHistoryWithNowBars("SystemDiskHistoryLayout", plotHeight, diskPlot, {readBar, writeBar}, false, STORAGE_NOW_BAR_COLUMNS);
        if (ctx.fill != nullptr)
        {
            ctx.fill->addPlot();
        }
    }
}

} // namespace App::StorageSection
