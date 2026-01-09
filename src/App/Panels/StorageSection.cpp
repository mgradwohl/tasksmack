#include "StorageSection.h"

#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
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
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

constexpr size_t STORAGE_NOW_BAR_COLUMNS = 2; // Read, Write

} // namespace

void updateSmoothedDiskIO(double targetRead, double targetWrite, float deltaTimeSeconds, RenderContext& ctx)
{
    if (ctx.smoothedReadBytesPerSec == nullptr || ctx.smoothedWriteBytesPerSec == nullptr || ctx.smoothedInitialized == nullptr)
    {
        return;
    }

    const double alpha = computeAlpha(deltaTimeSeconds, ctx.refreshInterval);

    if (!*ctx.smoothedInitialized)
    {
        *ctx.smoothedReadBytesPerSec = targetRead;
        *ctx.smoothedWriteBytesPerSec = targetWrite;
        *ctx.smoothedInitialized = true;
        return;
    }

    *ctx.smoothedReadBytesPerSec = smoothTowards(*ctx.smoothedReadBytesPerSec, targetRead, alpha);
    *ctx.smoothedWriteBytesPerSec = smoothTowards(*ctx.smoothedWriteBytesPerSec, targetWrite, alpha);
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
    const auto diskReadHist = ctx.storageModel->totalReadHistory();
    const auto diskWriteHist = ctx.storageModel->totalWriteHistory();
    const size_t alignedDisk = std::min({diskTimestamps.size(), diskReadHist.size(), diskWriteHist.size()});

    const auto diskAxis = alignedDisk > 0 ? makeTimeAxisConfig(diskTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds)
                                          : makeTimeAxisConfig({}, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    std::vector<float> diskTimes;
    std::vector<float> readData;
    std::vector<float> writeData;

    if (alignedDisk > 0)
    {
        diskTimes = buildTimeAxis(diskTimestamps, alignedDisk, nowSeconds);
        readData.reserve(alignedDisk);
        writeData.reserve(alignedDisk);
        // Copy from double to float
        for (size_t i = diskReadHist.size() - alignedDisk; i < diskReadHist.size(); ++i)
        {
            readData.push_back(static_cast<float>(diskReadHist[i]));
            writeData.push_back(static_cast<float>(diskWriteHist[i]));
        }
    }

    // Update smoothed I/O values
    updateSmoothedDiskIO(diskSnap.totalReadBytesPerSec, diskSnap.totalWriteBytesPerSec, ctx.lastDeltaSeconds, ctx);

    const double smoothedRead = ctx.smoothedReadBytesPerSec != nullptr ? *ctx.smoothedReadBytesPerSec : diskSnap.totalReadBytesPerSec;
    const double smoothedWrite = ctx.smoothedWriteBytesPerSec != nullptr ? *ctx.smoothedWriteBytesPerSec : diskSnap.totalWriteBytesPerSec;

    // Calculate max across all data for consistent Y axis
    const double diskMax = std::max({readData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(readData)),
                                     writeData.empty() ? 1.0 : static_cast<double>(*std::ranges::max_element(writeData)),
                                     smoothedRead,
                                     smoothedWrite,
                                     1.0});

    const NowBar readBar{.valueText = UI::Format::formatBytesPerSec(smoothedRead),
                         .label = "Disk Read",
                         .value01 = std::clamp(smoothedRead / diskMax, 0.0, 1.0),
                         .color = theme.scheme().chartCpu};
    const NowBar writeBar{.valueText = UI::Format::formatBytesPerSec(smoothedWrite),
                          .label = "Disk Write",
                          .value01 = std::clamp(smoothedWrite / diskMax, 0.0, 1.0),
                          .color = theme.accentColor(2)};

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
            plotLineWithFill("Read", diskTimes.data(), readData.data(), count, theme.scheme().chartCpu);
            plotLineWithFill("Write", diskTimes.data(), writeData.data(), count, theme.accentColor(2));

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
                        ImGui::TextColored(theme.scheme().chartCpu,
                                           "Read: %s",
                                           UI::Format::formatBytesPerSec(static_cast<double>(readData[*idxVal])).c_str());
                        ImGui::TextColored(theme.accentColor(2),
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

} // namespace App::StorageSection
