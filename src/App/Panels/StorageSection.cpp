#include "StorageSection.h"

#include "Domain/StorageModel.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace App::StorageSection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::formatAxisBytesPerSec;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

constexpr size_t STORAGE_NOW_BAR_COLUMNS = 2; // Read, Write

/// Render a single disk cell (label + read/write NowBars + chart).
void renderDiskCell(std::string_view deviceName,
                    const std::vector<float>& timeData,
                    const std::vector<float>& readData,
                    const std::vector<float>& writeData,
                    double currentRead,
                    double currentWrite,
                    const UI::Widgets::TimeAxisConfig& axisConfig,
                    const UI::Theme& theme)
{
    const double readMax = std::max({readData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(readData)), currentRead, 1.0});
    const double writeMax =
        std::max({writeData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(writeData)), currentWrite, 1.0});

    const NowBar readBar{.valueText = UI::Format::formatBytesPerSec(currentRead),
                         .label = "Read",
                         .value01 = std::clamp(currentRead / readMax, 0.0, 1.0),
                         .color = theme.scheme().chartIo};
    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(currentWrite),
                          .label = "Write",
                          .value01 = std::clamp(currentWrite / writeMax, 0.0, 1.0),
                          .color = theme.scheme().chartIoWrite};

    const std::string plotId = std::format("##DiskPlot_{}", deviceName);
    const std::string layoutId = std::format("DiskLayout_{}", deviceName);

    auto diskPlotFn = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
        {
            UI::Widgets::setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            const int count = UI::Format::checkedCount(timeData.size());
            plotLineWithFill("Read", timeData.data(), readData.data(), count, theme.scheme().chartIo, theme.scheme().chartIoFill);
            plotLineWithFill(
                "Write", timeData.data(), writeData.data(), count, theme.scheme().chartIoWrite, theme.scheme().chartIoWriteFill);

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
            ImPlot::EndPlot();
        }
    };

    ImGui::TextColored(theme.scheme().textPrimary, "%.*s", static_cast<int>(deviceName.size()), deviceName.data());
    renderHistoryWithNowBars(
        layoutId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, diskPlotFn, {readBar, writeBar}, false, STORAGE_NOW_BAR_COLUMNS);
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

    if (ctx.storageModel == nullptr)
    {
        ImGui::TextUnformatted("Storage model not available.");
        return;
    }

    const auto& diskSnap = ctx.storageModel->latestSnapshot();
    const auto diskTimestamps = ctx.storageModel->historyTimestamps();
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

    const auto perDisk = ctx.storageModel->perDiskHistory();
    const size_t diskCount = perDisk.size();

    if (diskCount > 1)
    {
        // ── Multi-disk: one chart cell per disk in a 2-column grid ──────────────
        ImGui::TextColored(
            theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  Disk I/O by Device (%zu disks, %zu samples)", diskCount, historySize);

        const float gridWidth = ImGui::GetContentRegionAvail().x;
        constexpr float MIN_CELL_WIDTH = 320.0F;
        const size_t gridCols = std::max<size_t>(1, static_cast<size_t>(gridWidth / MIN_CELL_WIDTH));
        const int gridColsInt = UI::Format::checkedCount(gridCols);
        const size_t gridRows = (diskCount + gridCols - 1) / gridCols;

        // Pre-build device name → snapshot lookup to avoid O(n²) linear scans in the cell loop.
        std::unordered_map<std::string, const Domain::DiskSnapshot*> diskLookup;
        diskLookup.reserve(diskSnap.disks.size());
        for (const auto& d : diskSnap.disks)
        {
            diskLookup.emplace(d.deviceName, &d);
        }

        if (ImGui::BeginTable("PerDiskGrid", gridColsInt, ImGuiTableFlags_SizingStretchSame))
        {
            for (size_t row = 0; row < gridRows; ++row)
            {
                ImGui::TableNextRow();
                for (size_t col = 0; col < gridCols; ++col)
                {
                    const size_t diskIdx = (row * gridCols) + col;
                    ImGui::TableNextColumn();
                    if (diskIdx >= diskCount)
                    {
                        continue;
                    }

                    const auto& disk = perDisk[diskIdx];
                    const size_t alignedCount = std::min({diskTimes.size(), disk.readBytesPerSec.size(), disk.writeBytesPerSec.size()});

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

                    const std::string childId = std::format("DiskCell_{}", disk.deviceName);
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
                    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);

                    const float labelHeight = ImGui::GetTextLineHeight();
                    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
                    constexpr float BAR_HEIGHT = 20.0F; // approximate NowBar row height
                    const float childHeight = labelHeight + spacingY + HISTORY_PLOT_HEIGHT_DEFAULT + BAR_HEIGHT + (spacingY * 2.0F);

                    if (ImGui::BeginChild(childId.c_str(), ImVec2(-FLT_MIN, childHeight), ImGuiChildFlags_Borders))
                    {
                        if (alignedCount == 0)
                        {
                            ImGui::TextColored(theme.scheme().textMuted, "%s\nCollecting data...", disk.deviceName.c_str());
                        }
                        else
                        {
                            const std::vector<float> cellTimes(diskTimes.end() - static_cast<std::ptrdiff_t>(alignedCount),
                                                               diskTimes.end());
                            renderDiskCell(disk.deviceName, cellTimes, readData, writeData, diskRead, diskWrite, diskAxis, theme);
                        }
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor(2);
                }
            }
            ImGui::EndTable();
        }
    }
    else
    {
        // ── Single disk (or no data yet): aggregate chart ─────────────────────
        const auto diskReadHist = ctx.storageModel->totalReadHistory();
        const auto diskWriteHist = ctx.storageModel->totalWriteHistory();
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
                             .value01 = std::clamp(smoothedRead / diskMax, 0.0, 1.0),
                             .color = theme.scheme().chartIo};
        const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(smoothedWrite),
                              .label = "Disk Write",
                              .value01 = std::clamp(smoothedWrite / diskMax, 0.0, 1.0),
                              .color = theme.scheme().chartIoWrite};

        auto diskPlot = [&]()
        {
            const UI::Widgets::PlotFontGuard fontGuard;
            if (ImPlot::BeginPlot("##SystemDiskHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), ImPlotFlags_NoMenus))
            {
                UI::Widgets::setupLegendDefault();
                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_AutoFit | Y_AXIS_FLAGS_DEFAULT);
                ImPlot::SetupAxisFormat(ImAxis_Y1, formatAxisBytesPerSec);
                ImPlot::SetupAxisLimits(ImAxis_X1, diskAxis.xMin, diskAxis.xMax, ImPlotCond_Always);

                const int count = UI::Format::checkedCount(alignedDisk);
                plotLineWithFill("Read", diskTimes.data(), readData.data(), count, theme.scheme().chartIo, theme.scheme().chartIoFill);
                plotLineWithFill(
                    "Write", diskTimes.data(), writeData.data(), count, theme.scheme().chartIoWrite, theme.scheme().chartIoWriteFill);

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

                ImPlot::EndPlot();
            }
        };

        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_HARD_DRIVE "  Disk I/O History (%zu samples)", alignedDisk);
        renderHistoryWithNowBars(
            "SystemDiskHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, diskPlot, {readBar, writeBar}, false, STORAGE_NOW_BAR_COLUMNS);
    }
}

} // namespace App::StorageSection
