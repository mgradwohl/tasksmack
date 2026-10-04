#include "MemorySection.h"

#include "Domain/SystemSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RateAxis.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace App::MemorySection
{

namespace
{

using UI::Widgets::computeAlpha;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBarList;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;

// One label per series, shared by its legend entry, tooltip row and NowBar (#1008).
constexpr const char* USED_LABEL = "Used";
constexpr const char* CACHED_LABEL = "Cached";
constexpr const char* SWAP_LABEL = "Swap";
constexpr const char* PEAK_LABEL = "Peak Used";

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

    const bool initialized = smoothed.initialized;
    smoothed.usedPercent = clampPercent(initializeOrSmooth(smoothed.usedPercent, targetMem, alpha, initialized));
    smoothed.cachedPercent = clampPercent(initializeOrSmooth(smoothed.cachedPercent, targetCached, alpha, initialized));
    smoothed.swapPercent = clampPercent(initializeOrSmooth(smoothed.swapPercent, targetSwap, alpha, initialized));
    smoothed.initialized = true;
}

void renderMemorySection(RenderContext& ctx, const std::vector<double>& timestamps, double nowSeconds, int nowBarColumns)
{
    if (ctx.publication == nullptr)
    {
        return;
    }

    const auto& theme = UI::Theme::get();
    const auto& snap = ctx.publication->snapshot;
    const auto axisConfig = makeTimeAxisConfig(timestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    // Get history data
    const auto& memHist = ctx.publication->memoryHistory;
    const auto& cachedHist = ctx.publication->memoryCachedHistory;
    const auto& swapHist = ctx.publication->swapHistory;

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

    const auto timeData = frameTimeAxis(timestamps, alignedCount, nowSeconds);
    const auto memData = UI::Widgets::tailAlignedSpan(memHist, alignedCount).values;
    const auto cachedData = UI::Widgets::tailAlignedSpan(cachedHist, alignedCount).values;
    const auto swapData = UI::Widgets::tailAlignedSpan(swapHist, alignedCount).values;

    // The peak of Used over the window, drawn as a reference line labelled PEAK_LABEL. It was
    // "##MemPeak": no legend entry and no tooltip row, so nothing said what the line was (#1007).
    const double peakMemPercent = UI::Widgets::maxOfSeries(memData);

    // "N% (used / total)" when the RAM total is known: physical RAM is fixed, so bytes back-calculated
    // from a historical percent are exact. Swap is percent-only: its size can change at runtime.
    const auto formatRamPercent = [&](double pct)
    {
        if (snap.memoryTotalBytes == 0)
        {
            return UI::Format::percentCompact(pct);
        }
        const auto bytes = static_cast<std::uint64_t>((pct / 100.0) * static_cast<double>(snap.memoryTotalBytes));
        return UI::Format::bytesUsedTotalPercentCompact(bytes, snap.memoryTotalBytes, pct);
    };

    auto memoryPlot = [&]()
    {
        const UI::Widgets::HistoryChart chart(UI::Widgets::withHeight(
            UI::Widgets::percentHistoryConfig("##MemorySwapHistory", axisConfig.xMin, axisConfig.xMax), ctx.plotHeight));
        if (chart.active())
        {
            UI::Widgets::drawCollectingHint(alignedCount); // The same "no data yet" state on every chart (#1013)
            if (!memData.empty())
            {
                plotLineWithFill(USED_LABEL,
                                 timeData.data(),
                                 memData.data(),
                                 UI::Format::checkedCount(memData.size()),
                                 theme.scheme().chartMemory,
                                 theme.scheme().chartMemoryFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            }

            if (!cachedData.empty())
            {
                plotLineWithFill(CACHED_LABEL,
                                 timeData.data(),
                                 cachedData.data(),
                                 UI::Format::checkedCount(cachedData.size()),
                                 theme.scheme().chartCpu,
                                 theme.scheme().chartCpuFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            }

            if (!swapData.empty())
            {
                plotLineWithFill(SWAP_LABEL,
                                 timeData.data(),
                                 swapData.data(),
                                 UI::Format::checkedCount(swapData.size()),
                                 theme.scheme().chartIo,
                                 theme.scheme().chartIoFill,
                                 2.0F,
                                 true,
                                 UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
            }

            if (peakMemPercent > 0.0)
            {
                const std::array<double, 2> xLine = {axisConfig.xMin, axisConfig.xMax};
                const std::array<double, 2> yLine = {peakMemPercent, peakMemPercent};
                ImPlot::PlotLine(
                    PEAK_LABEL,
                    xLine.data(),
                    yLine.data(),
                    2,
                    {ImPlotProp_LineColor, theme.scheme().chartPeakLine, ImPlotProp_LineWeight, UI::Widgets::lineWeight(1.5F)});
            }

            if (ImPlot::IsPlotHovered())
            {
                const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                {
                    std::vector<UI::Widgets::TooltipRow> rows;
                    if (*idxVal < memData.size())
                    {
                        rows.push_back({.label = USED_LABEL,
                                        .color = theme.scheme().chartMemory,
                                        .value = formatRamPercent(static_cast<double>(memData[*idxVal]))});
                    }
                    if (*idxVal < cachedData.size())
                    {
                        rows.push_back({.label = CACHED_LABEL,
                                        .color = theme.scheme().chartCpu,
                                        .value = formatRamPercent(static_cast<double>(cachedData[*idxVal]))});
                    }
                    if (*idxVal < swapData.size())
                    {
                        rows.push_back({.label = SWAP_LABEL,
                                        .color = theme.scheme().chartIo,
                                        .value = UI::Format::percentCompact(static_cast<double>(swapData[*idxVal]))});
                    }
                    if (peakMemPercent > 0.0)
                    {
                        rows.push_back({.label = PEAK_LABEL,
                                        .color = theme.scheme().chartPeakLine,
                                        .value = UI::Format::percentCompact(peakMemPercent)});
                    }
                    UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                }
            }
        }
    };

    // A bar for every series the chart draws, and none for one it does not (#1006). Bars used to be
    // dropped when the RAM or swap total was 0 while their series, legend entry and tooltip row stayed.
    // The bar's height and value are the smoothed percent; its tooltip has the current sample's bytes,
    // so the byte and percent figures come from the same sample.
    NowBarList memoryBars;
    if (ctx.smoothedMemory != nullptr)
    {
        const auto addBar = [&](const char* label, double smoothedPercent, std::string tooltip, const ImVec4& color)
        {
            const double clamped = std::clamp(smoothedPercent, 0.0, 100.0);
            memoryBars.push_back({.valueText = UI::Format::percentCompact(clamped),
                                  .label = label,
                                  .tooltipText = std::move(tooltip),
                                  .value01 = UI::Format::percent01(clamped),
                                  .color = color});
        };
        const auto ramTooltip = [&](const char* label, std::uint64_t bytes, double pct)
        {
            return snap.memoryTotalBytes > 0
                     ? UI::Widgets::formatTooltipRow(label, UI::Format::bytesUsedTotalPercentCompact(bytes, snap.memoryTotalBytes, pct))
                     : std::string{};
        };
        if (!memData.empty())
        {
            addBar(USED_LABEL,
                   ctx.smoothedMemory->usedPercent,
                   ramTooltip(USED_LABEL, snap.memoryUsedBytes, snap.memoryUsedPercent),
                   theme.scheme().chartMemory);
        }
        if (!cachedData.empty())
        {
            addBar(CACHED_LABEL,
                   ctx.smoothedMemory->cachedPercent,
                   ramTooltip(CACHED_LABEL, snap.memoryCachedBytes, snap.memoryCachedPercent),
                   theme.scheme().chartCpu);
        }
        if (!swapData.empty())
        {
            addBar(SWAP_LABEL, ctx.smoothedMemory->swapPercent, {}, theme.scheme().chartIo);
        }
    }

    renderHistoryWithNowBars("MemorySwapHistoryLayout", ctx.plotHeight, memoryPlot, memoryBars, false, static_cast<size_t>(nowBarColumns));
}

} // namespace App::MemorySection
