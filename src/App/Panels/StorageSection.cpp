#include "StorageSection.h"

#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "UI/ChartGrid.h"
#include "UI/ChartGridLayout.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/HistoryPlotHeight.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace App::StorageSection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::ChartGridConfig;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::normalizeToUnitInterval;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderChartGrid;
using UI::Widgets::renderHistoryWithNowBars;

constexpr size_t STORAGE_NOW_BAR_COLUMNS = 2; // Read, Write

// Narrowest a disk cell may get before the grid uses fewer columns instead, in ems.
constexpr float MIN_DISK_CELL_WIDTH_EM = 30.0F;

/// Minimum plot height a disk cell will shrink to before the grid prefers scrolling over squashing
/// charts flat. The same font-relative floor the Overview's charts hold (UI/HistoryPlotHeight.h),
/// in whole pixels, so both bounds of the shared height rule apply here and not only the ceiling.
/// It was a fixed 60px, which at Even Huger is under three lines of axis text.
[[nodiscard]] float minDiskPlotHeight()
{
    return std::floor(UI::Widgets::historyPlotMinHeight(ImGui::GetFontSize()));
}

/// Render a single disk cell (label + read/write NowBars + chart). cellHeight is the enclosing
/// grid cell's *usable content* height (see renderChartGrid's cellWidth/cellHeight doc in
/// ChartGrid.h -- it's measured via GetContentRegionAvail() inside the cell's BeginChild, not
/// the outer size passed to it); the plot height is derived from it by measuring the label row's
/// actual consumed height via cursor position (rather than guessing at ImGui's spacing rules
/// with a hand-picked constant -- see #823 review) so the chart fills exactly what's left in the
/// cell.
///
/// cachedOverhead is measured once per frame (on the first disk) and reused for the rest, and
/// cached across frames too until the style metrics it's built from change: every cell gets the
/// same cellHeight (ImGuiTableFlags_SizingStretchSame) and renders an identically-shaped
/// single-line label row, so the resulting vertical overhead is the same across all disks and
/// doesn't change frame to frame on its own.
void renderDiskCell(const std::string& deviceName,
                    const std::vector<float>& timeData,
                    const std::vector<float>& readData,
                    const std::vector<float>& writeData,
                    double currentRead,
                    double currentWrite,
                    const UI::Widgets::TimeAxisConfig& axisConfig,
                    const UI::Theme& theme,
                    float cellHeight,
                    std::optional<float>& cachedOverhead)
{
    // A per-disk series holds NaN for samples where the disk was absent, and currentRead/Write are
    // NaN when it is absent from the latest sample (#1015). maxOfSeries, not max_element, whose
    // answer depends on where a NaN sits; and the bars show N/A rather than a false 0 B/s, as the
    // GPU fan bar does for an unreadable sample.
    const auto finiteOrZero = [](double value)
    {
        return std::isfinite(value) ? value : 0.0;
    };
    const double diskMax =
        std::max({UI::Widgets::maxOfSeries(readData, writeData), finiteOrZero(currentRead), finiteOrZero(currentWrite), 1.0});

    const auto makeBar = [&](const char* label, double current, const ImVec4& color)
    {
        if (!std::isfinite(current))
        {
            return NowBar{.valueText = "N/A",
                          .label = label,
                          .tooltipText = std::format("{}: not reported this sample", label),
                          .value01 = 0.0,
                          .color = theme.scheme().textMuted};
        }
        return NowBar{.valueText = UI::Format::formatBytesPerSec(current),
                      .label = label,
                      .tooltipText = {},
                      .value01 = normalizeToUnitInterval(current, diskMax),
                      .color = color};
    };
    const NowBar readBar = makeBar("Read", currentRead, theme.scheme().chartIo);
    const NowBar writeBar = makeBar("Write", currentWrite, theme.scheme().chartIoWrite);

    const float cellContentTop = ImGui::GetCursorPosY();
    ImGui::TextColored(theme.scheme().textPrimary, "%.*s", static_cast<int>(deviceName.size()), deviceName.data());
    if (!cachedOverhead.has_value())
    {
        // renderHistoryWithNowBars wraps the chart+bars in its own table, whose CellPadding.y
        // (top+bottom) adds a little more height beyond the label -- account for it here rather
        // than clipping the chart against it (#823 review: residual scrollbar after the cell's own
        // WindowPadding was already corrected for).
        cachedOverhead = (ImGui::GetCursorPosY() - cellContentTop) + (ImGui::GetStyle().CellPadding.y * 2.0F);
    }
    const float measuredOverhead = *cachedOverhead;
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
        auto diskCfg = UI::Widgets::rateHistoryConfig(deviceName.c_str(),
                                                      axisConfig.xMin,
                                                      axisConfig.xMax,
                                                      formatAxisBytesPerSec,
                                                      UI::Widgets::maxOfSeries(readData, writeData),
                                                      UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC);
        diskCfg.flags |= ImPlotFlags_NoTitle;
        diskCfg.height = plotHeight;
        const UI::Widgets::HistoryChart chart(diskCfg);
        if (chart.active())
        {
            const int count = UI::Format::checkedCount(timeData.size());
            plotLineWithFill("Read",
                             timeData.data(),
                             readData.data(),
                             count,
                             theme.scheme().chartIo,
                             theme.scheme().chartIoFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            plotLineWithFill("Write",
                             timeData.data(),
                             writeData.data(),
                             count,
                             theme.scheme().chartIoWrite,
                             theme.scheme().chartIoWriteFill,
                             2.0F,
                             true,
                             UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

            if (ImPlot::IsPlotHovered() && !timeData.empty())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    if (*idxVal < timeData.size())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted(formatAgeSeconds(static_cast<double>(timeData[*idxVal])).c_str());
                        ImGui::Separator();
                        ImGui::TextColored(theme.scheme().chartIo,
                                           "Read: %s",
                                           UI::Format::formatBytesPerSecOrNA(static_cast<double>(readData[*idxVal])).c_str());
                        ImGui::TextColored(theme.scheme().chartIoWrite,
                                           "Write: %s",
                                           UI::Format::formatBytesPerSecOrNA(static_cast<double>(writeData[*idxVal])).c_str());
                        ImGui::EndTooltip();
                    }
                }
            }
        }
    };

    // deviceName is a real std::string (not string_view), so .c_str() is a guaranteed
    // null-terminated C-string at zero extra cost -- reusing it as the RenderMetrics/table id
    // keeps per-disk RenderMetrics entries from collapsing into one (#823 review), without
    // reintroducing the per-frame heap allocation a formatted id had.
    renderHistoryWithNowBars(deviceName.c_str(), plotHeight, diskPlotFn, {readBar, writeBar}, false, STORAGE_NOW_BAR_COLUMNS);
}

} // namespace

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
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    if (ctx.publication == nullptr)
    {
        ImGui::TextUnformatted("Storage model not available.");
        return;
    }

    const auto& diskSnap = ctx.publication->snapshot;
    const auto& diskTimestamps = ctx.publication->timestamps;
    const size_t historySize = diskTimestamps.size();

    const auto diskAxis = historySize > 0 ? makeTimeAxisConfig(diskTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                          : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Build shared time axis (float, relative)
    std::vector<float> diskTimes;
    if (historySize > 0)
    {
        diskTimes = buildTimeAxis(diskTimestamps, historySize, nowSeconds);
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

        // Pre-build device name → snapshot lookup to avoid O(n²) linear scans in the cell loop.
        std::unordered_map<std::string, const Domain::DiskSnapshot*> diskLookup;
        diskLookup.reserve(diskSnap.disks.size());
        for (const auto& d : diskSnap.disks)
        {
            diskLookup.emplace(d.deviceName, &d);
        }

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
        const float approxLabelOverhead = (ImGui::GetStyle().WindowPadding.y * 2.0F) + ImGui::GetTextLineHeight() +
                                          ImGui::GetStyle().ItemSpacing.y + (ImGui::GetStyle().CellPadding.y * 2.0F);

        // Measured once (by renderDiskCell, on the first disk) and reused for the rest -- see
        // renderDiskCell's doc comment. Cached across frames too, not just across disks within
        // one frame: remeasure only when the style values it's built from actually change.
        //
        // Keyed on the actual style values (text line height, ItemSpacing.y, CellPadding.y)
        // rather than theme.currentFontSize() alone: today's theme switches happen to leave
        // those metrics untouched (Theme::applyImGuiStyle sets them to fixed values independent
        // of the color scheme), but that's a property of the current theme implementation, not
        // something this cache should have to assume stays true (#823 review).
        static std::optional<float> cachedOverhead;
        static float cachedTextLineHeight = -1.0F;
        static float cachedItemSpacingY = -1.0F;
        static float cachedCellPaddingY = -1.0F;
        // Epsilon rather than `==`/`!=` on floats (CodeQL cpp/equality-on-floats): these are
        // stored style values, not accumulated arithmetic, so exact comparison would actually be
        // safe here, but a tolerance costs nothing and avoids relying on that.
        constexpr float STYLE_METRIC_EPSILON = 1e-4F;
        if (const float textLineHeight = ImGui::GetTextLineHeight(),
            itemSpacingY = ImGui::GetStyle().ItemSpacing.y,
            cellPaddingY = ImGui::GetStyle().CellPadding.y;
            std::abs(cachedTextLineHeight - textLineHeight) > STYLE_METRIC_EPSILON ||
            std::abs(cachedItemSpacingY - itemSpacingY) > STYLE_METRIC_EPSILON ||
            std::abs(cachedCellPaddingY - cellPaddingY) > STYLE_METRIC_EPSILON)
        {
            cachedOverhead.reset();
            cachedTextLineHeight = textLineHeight;
            cachedItemSpacingY = itemSpacingY;
            cachedCellPaddingY = cellPaddingY;
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ChartGridConfig gridConfig{
            .availableWidth = avail.x,
            .availableHeight = avail.y,
            // 30 em is the former fixed 320px at the reference em (32/3 px): the width floor now
            // scales with the font like the height floor beside it, so a large font gets fewer,
            // wider cells rather than rate labels and NowBars crowding a fixed 320px (#964).
            .minCellWidth = MIN_DISK_CELL_WIDTH_EM * ImGui::GetFontSize(),
            // The same floor and ceiling the Overview's charts keep to (UI/HistoryPlotHeight.h), so the
            // two tabs follow one rule instead of one never growing and the other never stopping (#923).
            .minCellHeight = approxLabelOverhead + minDiskPlotHeight(),
            .maxCellHeight = approxLabelOverhead + UI::Widgets::historyPlotMaxHeight(ImGui::GetFontSize()),
        };

        renderChartGrid(
            "PerDiskGrid",
            diskCount,
            gridConfig,
            [&](const size_t diskIdx, float /*cellWidth*/, const float cellHeight)
            {
                const auto& disk = perDisk[diskIdx];
                const size_t alignedCount = std::min({diskTimes.size(), disk.readBytesPerSec.size(), disk.writeBytesPerSec.size()});

                if (alignedCount == 0)
                {
                    ImGui::TextColored(theme.scheme().textMuted, "%s\nCollecting data...", disk.deviceName.c_str());
                    return;
                }

                // Build float read/write data for this disk
                std::vector<float> readData;
                std::vector<float> writeData;
                readData.reserve(alignedCount);
                writeData.reserve(alignedCount);
                const size_t readOffset = disk.readBytesPerSec.size() - alignedCount;
                const size_t writeOffset = disk.writeBytesPerSec.size() - alignedCount;
                for (size_t i = 0; i < alignedCount; ++i)
                {
                    readData.push_back(static_cast<float>(disk.readBytesPerSec[readOffset + i]));
                    writeData.push_back(static_cast<float>(disk.writeBytesPerSec[writeOffset + i]));
                }

                // Per-disk snapshot values for NowBars (O(1) lookup via pre-built map).
                // NaN if the disk is missing from the latest sample: renderDiskCell shows N/A, not 0.
                double diskRead = std::numeric_limits<double>::quiet_NaN();
                double diskWrite = std::numeric_limits<double>::quiet_NaN();
                if (const auto it = diskLookup.find(disk.deviceName); it != diskLookup.end())
                {
                    diskRead = it->second->readBytesPerSec;
                    diskWrite = it->second->writeBytesPerSec;
                }

                const std::vector<float> cellTimes(diskTimes.end() - static_cast<std::ptrdiff_t>(alignedCount), diskTimes.end());
                renderDiskCell(
                    disk.deviceName, cellTimes, readData, writeData, diskRead, diskWrite, diskAxis, theme, cellHeight, cachedOverhead);
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

        std::vector<float> readData;
        std::vector<float> writeData;
        if (alignedDisk > 0)
        {
            readData.reserve(alignedDisk);
            writeData.reserve(alignedDisk);
            for (size_t i = diskReadHist.size() - alignedDisk; i < diskReadHist.size(); ++i)
            {
                readData.push_back(static_cast<float>(diskReadHist[i]));
                writeData.push_back(static_cast<float>(diskWriteHist[i]));
            }
        }

        // Calculate max across all data for consistent Y axis
        const double diskMax = std::max({readData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(readData)),
                                         writeData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(writeData)),
                                         smoothedRead,
                                         smoothedWrite,
                                         1.0});

        const NowBar readBar{.valueText = UI::Format::formatBytesPerSec(smoothedRead),
                             .label = "Disk Read",
                             .tooltipText = {},
                             .value01 = std::clamp(smoothedRead / diskMax, 0.0, 1.0),
                             .color = theme.scheme().chartIo};
        const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(smoothedWrite),
                              .label = "Disk Write",
                              .tooltipText = {},
                              .value01 = std::clamp(smoothedWrite / diskMax, 0.0, 1.0),
                              .color = theme.scheme().chartIoWrite};

        // Shares the tab's height with the network chart above it (#959).
        const float plotHeight = (ctx.fill != nullptr) ? ctx.fill->plotHeight() : HISTORY_PLOT_HEIGHT_DEFAULT;
        auto diskPlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(
                UI::Widgets::withHeight(UI::Widgets::rateHistoryConfig("##SystemDiskHistory",
                                                                       diskAxis.xMin,
                                                                       diskAxis.xMax,
                                                                       formatAxisBytesPerSec,
                                                                       UI::Widgets::maxOfSeries(readData, writeData),
                                                                       UI::Widgets::RATE_AXIS_MIN_SPAN_BYTES_PER_SEC),
                                        plotHeight));
            if (chart.active())
            {
                const int count = UI::Format::checkedCount(alignedDisk);
                plotLineWithFill("Read",
                                 diskTimes.data(),
                                 readData.data(),
                                 count,
                                 theme.scheme().chartIo,
                                 theme.scheme().chartIoFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                plotLineWithFill("Write",
                                 diskTimes.data(),
                                 writeData.data(),
                                 count,
                                 theme.scheme().chartIoWrite,
                                 theme.scheme().chartIoWriteFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

                if (ImPlot::IsPlotHovered())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(diskTimes, mouse.x))
                    {
                        if (*idxVal < alignedDisk)
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(static_cast<double>(diskTimes[*idxVal]));
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::Separator();
                            ImGui::TextColored(theme.scheme().chartIo,
                                               "Read: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(readData[*idxVal])).c_str());
                            ImGui::TextColored(theme.scheme().chartIoWrite,
                                               "Write: %s",
                                               UI::Format::formatBytesPerSec(static_cast<double>(writeData[*idxVal])).c_str());
                            ImGui::EndTooltip();
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
