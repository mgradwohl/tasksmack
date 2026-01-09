#include "MemorySection.h"

#include "Domain/SystemSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <vector>

namespace App::MemorySection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::cropFrontToSize;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::PLOT_FLAGS_DEFAULT;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;
using UI::Widgets::X_AXIS_FLAGS_DEFAULT;
using UI::Widgets::Y_AXIS_FLAGS_DEFAULT;

} // namespace

void updateSmoothedMemory(SmoothedMemory& smoothed,
                          const Domain::SystemSnapshot& snap,
                          float deltaTimeSeconds,
                          std::chrono::milliseconds refreshInterval)
{
    using UI::Format::clampPercent;

    const double alpha = computeAlpha(deltaTimeSeconds, refreshInterval);

    const double targetMem = clampPercent(snap.memoryUsedPercent);
    const double targetCached = clampPercent(snap.memoryCachedPercent);
    const double targetSwap = clampPercent(snap.swapUsedPercent);

    if (!smoothed.initialized)
    {
        smoothed.usedPercent = targetMem;
        smoothed.cachedPercent = targetCached;
        smoothed.swapPercent = targetSwap;
        smoothed.initialized = true;
        return;
    }

    smoothed.usedPercent = clampPercent(smoothTowards(smoothed.usedPercent, targetMem, alpha));
    smoothed.cachedPercent = clampPercent(smoothTowards(smoothed.cachedPercent, targetCached, alpha));
    smoothed.swapPercent = clampPercent(smoothTowards(smoothed.swapPercent, targetSwap, alpha));
}

void renderMemorySection(RenderContext& ctx, const std::vector<double>& timestamps, double nowSeconds, int nowBarColumns)
{
    if (ctx.systemModel == nullptr)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const auto snap = ctx.systemModel->snapshot();
    const auto axisConfig = makeTimeAxisConfig(timestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Get history data
    auto memHist = ctx.systemModel->memoryHistory();
    auto cachedHist = ctx.systemModel->memoryCachedHistory();
    auto swapHist = ctx.systemModel->swapHistory();
    double peakMemPercent = 0.0;

    ImGui::TextColored(
        theme.scheme().textPrimary, ICON_FA_MEMORY "  Memory & Swap (%zu samples)", std::min(memHist.size(), timestamps.size()));
    ImGui::Spacing();

    const size_t memCount = std::min(memHist.size(), timestamps.size());
    const size_t cachedCount = std::min(cachedHist.size(), timestamps.size());
    const size_t swapCount = std::min(swapHist.size(), timestamps.size());

    size_t alignedCount = memCount;
    if (cachedCount > 0)
    {
        alignedCount = std::min(alignedCount, cachedCount);
    }
    if (swapCount > 0)
    {
        alignedCount = std::min(alignedCount, swapCount);
    }

    cropFrontToSize(memHist, alignedCount);
    cropFrontToSize(cachedHist, std::min(cachedCount, alignedCount));
    cropFrontToSize(swapHist, std::min(swapCount, alignedCount));
    std::vector<float> timeData = buildTimeAxis(timestamps, alignedCount, nowSeconds);

    auto memoryPlot = [&]()
    {
        const UI::Widgets::PlotFontGuard fontGuard;
        if (ImPlot::BeginPlot("##MemorySwapHistory", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
        {
            UI::Widgets::setupLegendDefault();
            ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
            ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_X1, axisConfig.xMin, axisConfig.xMax, ImPlotCond_Always);

            if (!memHist.empty())
            {
                plotLineWithFill(
                    "Used", timeData.data(), memHist.data(), UI::Format::checkedCount(memHist.size()), theme.scheme().chartMemory);
                peakMemPercent = static_cast<double>(*std::ranges::max_element(memHist));
            }

            if (!cachedHist.empty())
            {
                plotLineWithFill(
                    "Cached", timeData.data(), cachedHist.data(), UI::Format::checkedCount(cachedHist.size()), theme.scheme().chartCpu);
            }

            if (!swapHist.empty())
            {
                plotLineWithFill(
                    "Swap", timeData.data(), swapHist.data(), UI::Format::checkedCount(swapHist.size()), theme.scheme().chartIo);
            }

            // Peak memory reference line
            if (peakMemPercent > 0.0)
            {
                const float peak = UI::Format::toFloatNarrow(peakMemPercent);
                const float xLine[2] = {UI::Format::toFloatNarrow(axisConfig.xMin), UI::Format::toFloatNarrow(axisConfig.xMax)};
                const float yLine[2] = {peak, peak};
                ImPlot::SetNextLineStyle(theme.scheme().textWarning, 1.5F);
                ImPlot::PlotLine("##MemPeak", xLine, yLine, 2);
            }

            // Tooltip on hover
            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    ImGui::BeginTooltip();
                    const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                    ImGui::TextUnformatted(ageText.c_str());

                    if (*idxVal < memHist.size())
                    {
                        ImGui::TextColored(theme.scheme().chartMemory, "Used: %s", UI::Format::percentCompact(memHist[*idxVal]).c_str());
                    }
                    if (*idxVal < cachedHist.size())
                    {
                        ImGui::TextColored(theme.scheme().chartCpu, "Cached: %s", UI::Format::percentCompact(cachedHist[*idxVal]).c_str());
                    }
                    if (*idxVal < swapHist.size())
                    {
                        ImGui::TextColored(theme.scheme().chartIo, "Swap: %s", UI::Format::percentCompact(swapHist[*idxVal]).c_str());
                    }
                    ImGui::EndTooltip();
                }
            }

            ImPlot::EndPlot();
        }
    };

    // Build now bars for current values
    std::vector<NowBar> memoryBars;
    if (ctx.smoothedMemory != nullptr && snap.memoryTotalBytes > 0)
    {
        const double usedPercentClamped = std::clamp(ctx.smoothedMemory->usedPercent, 0.0, 100.0);
        memoryBars.push_back({.valueText = UI::Format::percentCompact(usedPercentClamped),
                              .label = "Memory Used",
                              .value01 = UI::Format::percent01(usedPercentClamped),
                              .color = theme.scheme().chartMemory});

        const double cachedPercentClamped = std::clamp(ctx.smoothedMemory->cachedPercent, 0.0, 100.0);
        memoryBars.push_back({.valueText = UI::Format::percentCompact(cachedPercentClamped),
                              .label = "Memory Cached",
                              .value01 = UI::Format::percent01(cachedPercentClamped),
                              .color = theme.scheme().chartCpu});
    }

    if (ctx.smoothedMemory != nullptr && snap.swapTotalBytes > 0)
    {
        const double swapPercentClamped = std::clamp(ctx.smoothedMemory->swapPercent, 0.0, 100.0);
        memoryBars.push_back({.valueText = UI::Format::percentCompact(swapPercentClamped),
                              .label = "Swap Used",
                              .value01 = UI::Format::percent01(swapPercentClamped),
                              .color = theme.scheme().chartIo});
    }

    renderHistoryWithNowBars(
        "MemorySwapHistoryLayout", HISTORY_PLOT_HEIGHT_DEFAULT, memoryPlot, memoryBars, false, static_cast<size_t>(nowBarColumns));
}

} // namespace App::MemorySection
