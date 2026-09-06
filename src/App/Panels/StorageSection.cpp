#include "StorageSection.h"

#include "Domain/StorageModel.h"
#include "Domain/StorageSnapshot.h"
#include "UI/ChartGrid.h"
#include "UI/ChartGridLayout.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
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

// Minimum plot height a disk cell will shrink to before the grid prefers scrolling over
// squashing charts flat -- mirrors CpuCoresSection's MIN_PLOT_HEIGHT.
constexpr float MIN_PLOT_HEIGHT = 60.0F;

/// Render a single disk cell (label + read/write NowBars + chart). cellHeight is the enclosing
/// grid cell's full size (see renderChartGrid in renderStorageSection); the plot height is
/// derived from it by measuring the label row's actual consumed height via cursor position
/// (rather than guessing at ImGui's spacing rules with a hand-picked constant -- see #823
/// review) so the chart fills exactly what's left in the cell.
///
/// cachedOverhead is measured once (on the first disk of the frame) and reused for the rest:
/// every cell gets the same cellHeight (ImGuiTableFlags_SizingStretchSame) and renders an
/// identically-shaped single-line label row, so the resulting vertical overhead is the same
/// across all disks -- no need to repeat the cursor-position measurement per disk, per frame.
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
    const double diskMax = std::max({readData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(readData)),
                                     writeData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(writeData)),
                                     currentRead,
                                     currentWrite,
                                     1.0});

    const NowBar readBar{.valueText = UI::Format::formatBytesPerSec(currentRead),
                         .label = "Read",
                         .tooltipText = {},
                         .value01 = normalizeToUnitInterval(currentRead, diskMax),
                         .color = theme.scheme().chartIo};
    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(currentWrite),
                          .label = "Write",
                          .tooltipText = {},
                          .value01 = normalizeToUnitInterval(currentWrite, diskMax),
                          .color = theme.scheme().chartIoWrite};

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
    const float plotHeight = std::max(MIN_PLOT_HEIGHT, cellHeight - measuredOverhead);

    auto diskPlotFn = [&]()
    {
        auto diskCfg = UI::Widgets::autoFitHistoryConfig("##DiskPlot", axisConfig.xMin, axisConfig.xMax, formatAxisBytesPerSec);
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

    if (diskCount > 1)
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
        const float approxLabelOverhead = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;

        // Measured once (by renderDiskCell, on the first disk) and reused for the rest -- see
        // renderDiskCell's doc comment. Cached across frames too, not just across disks within
        // one frame: this value only changes when the font size does, so remeasure only then.
        static std::optional<float> cachedOverhead;
        static UI::FontSize cachedOverheadFontSize = UI::FontSize::Count;
        if (const UI::FontSize currentFontSize = theme.currentFontSize(); cachedOverheadFontSize != currentFontSize)
        {
            cachedOverhead.reset();
            cachedOverheadFontSize = currentFontSize;
        }

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ChartGridConfig gridConfig{
            .availableWidth = avail.x,
            .availableHeight = avail.y,
            .minCellWidth = 320.0F,
            .minCellHeight = approxLabelOverhead + MIN_PLOT_HEIGHT,
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
                double diskRead = 0.0;
                double diskWrite = 0.0;
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

        auto diskPlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(
                UI::Widgets::autoFitHistoryConfig("##SystemDiskHistory", diskAxis.xMin, diskAxis.xMax, formatAxisBytesPerSec));
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
        renderHistoryWithNowBars(
            "SystemDiskHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, diskPlot, {readBar, writeBar}, false, STORAGE_NOW_BAR_COLUMNS);
    }
}

} // namespace App::StorageSection
