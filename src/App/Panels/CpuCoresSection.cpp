#include "CpuCoresSection.h"

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"
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
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace App::CpuCoresSection
{

namespace
{

using UI::Widgets::BAR_WIDTH;
using UI::Widgets::buildTimeAxis;
using UI::Widgets::ChartGridConfig;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderChartGrid;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::smoothTowards;

// Minimum plot height a core cell will shrink to before the grid prefers scrolling over
// squashing charts flat -- keeps the chart legible even on a very short window with many rows.
constexpr float MIN_PLOT_HEIGHT = 60.0F;

} // namespace

void renderCpuCoresSection(RenderContext& ctx)
{
    if (ctx.publication == nullptr)
    {
        ImGui::TextUnformatted("System model not available");
        return;
    }

    const auto& snap = ctx.publication->snapshot;
    const auto& perCoreHist = ctx.publication->perCoreHistory;
    auto& theme = UI::Theme::get();

    // CPU model header
    std::string coreInfo;
    if (snap.cpuFreqMHz > 0)
    {
        coreInfo = std::format(" ({} cores @ {:.2f} GHz)", snap.coreCount, Domain::Numeric::toDouble(snap.cpuFreqMHz) / 1000.0);
    }
    else
    {
        coreInfo = std::format(" ({} cores)", snap.coreCount);
    }
    ImGui::TextUnformatted(snap.cpuModel.c_str());
    ImGui::SameLine(0, 0);
    ImGui::TextUnformatted(coreInfo.c_str());
    ImGui::Spacing();

    const size_t numCores = snap.cpuPerCore.size();
    if (numCores == 0)
    {
        ImGui::TextColored(theme.scheme().textMuted, "No per-core data available");
        return;
    }

    updateSmoothedPerCore(snap, ctx);

    // Get timestamps from cache or model
    const auto& timestamps = ctx.publication->timestamps;
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto axisConfig = makeTimeAxisConfig(timestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    if (perCoreHist.empty() || timestamps.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting data...");
        return;
    }

    const size_t coreCount = perCoreHist.size();

    // Grid layout: fills the full available panel space (width and height), choosing a
    // rows x columns shape that tracks the panel's own aspect ratio (square panel -> square-ish
    // grid, wide panel -> fewer rows/more columns) -- see UI/ChartGridLayout.h for the sizing
    // algorithm shared with StorageSection's per-disk grid.
    {
        // Approximate overhead used only as a floor for the grid's minimum cell height (so the
        // layout algorithm has a reasonable size to reason about before any cell has actually
        // rendered). The real per-cell overhead is measured directly below via cursor position,
        // since the label row's true height depends on ImGui's own spacing rules and isn't
        // worth re-deriving by hand (see #823 review: an earlier version of this guessed at that
        // math, including a horizontal NowBar-column constant that doesn't belong in a vertical
        // measurement at all, and ended up over-subtracting from the plot height).
        const float approxLabelOverhead = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float barColumnAllowance = BAR_WIDTH; // extra width renderHistoryWithNowBars reserves for the NowBar column

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ChartGridConfig gridConfig{
            .availableWidth = avail.x,
            .availableHeight = avail.y,
            .minCellWidth = 240.0F + barColumnAllowance,
            .minCellHeight = approxLabelOverhead + MIN_PLOT_HEIGHT,
        };

        // Every cell gets the same cellHeight (ImGuiTableFlags_SizingStretchSame) and renders an
        // identically-shaped label row (same font, same one Spacing() call, same wrapping
        // table), so the resulting vertical overhead -- and therefore plotHeight -- is identical
        // across all coreCount cells and doesn't change frame to frame unless the font size does.
        // Cache it across frames (not just across cells within one frame) so it's remeasured only
        // when that actually happens, not on every single frame regardless.
        static std::optional<float> cachedOverhead;
        static UI::FontSize cachedOverheadFontSize = UI::FontSize::Count;
        if (const UI::FontSize currentFontSize = theme.currentFontSize(); cachedOverheadFontSize != currentFontSize)
        {
            cachedOverhead.reset();
            cachedOverheadFontSize = currentFontSize;
        }

        renderChartGrid("PerCoreGrid",
                        coreCount,
                        gridConfig,
                        [&](const size_t coreIdx, float /*cellWidth*/, const float cellHeight)
                        {
                            const auto& samples = perCoreHist[coreIdx];
                            if (samples.empty())
                            {
                                ImGui::TextColored(theme.scheme().textMuted, "Core %zu\nCollecting data...", coreIdx);
                                return;
                            }

                            const float cellContentTop = ImGui::GetCursorPosY();
                            const std::string coreLabel = std::format(ICON_FA_MICROCHIP " Core {}", coreIdx);
                            const float availableWidth = ImGui::GetContentRegionAvail().x;
                            const float labelWidth = ImGui::CalcTextSize(coreLabel.c_str()).x;
                            const float labelOffset = std::max(0.0F, (availableWidth - labelWidth) * 0.5F);
                            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelOffset);
                            ImGui::TextUnformatted(coreLabel.c_str());
                            ImGui::Spacing();
                            if (!cachedOverhead.has_value())
                            {
                                // renderHistoryWithNowBars wraps the chart+bar in its own table, whose
                                // CellPadding.y (top+bottom) compactSpacing doesn't zero (only the
                                // horizontal padding) -- account for it here rather than clipping the
                                // chart against it (#823 review: residual scrollbar after the cell's
                                // own WindowPadding was already corrected for).
                                cachedOverhead = (ImGui::GetCursorPosY() - cellContentTop) + (ImGui::GetStyle().CellPadding.y * 2.0F);
                            }
                            const float measuredOverhead = *cachedOverhead;

                            std::vector<float> timeData = buildTimeAxis(timestamps, samples.size(), nowSeconds);
                            const float plotHeight = std::max(MIN_PLOT_HEIGHT, cellHeight - measuredOverhead);

                            // Capture necessary variables by value/reference for lambda
                            const auto& sampleData = samples;
                            const auto& themeRef = theme;
                            const auto& axisCfg = axisConfig;

                            auto plotFn = [&timeData, &sampleData, &themeRef, &axisCfg, plotHeight]()
                            {
                                auto coreCfg = UI::Widgets::percentHistoryConfig("##PerCorePlot", axisCfg.xMin, axisCfg.xMax);
                                coreCfg.showLegend = false;
                                coreCfg.height = plotHeight;
                                const UI::Widgets::HistoryChart chart(coreCfg);
                                if (chart.active())
                                {
                                    plotLineWithFill("##Core",
                                                     timeData.data(),
                                                     sampleData.data(),
                                                     UI::Format::checkedCount(timeData.size()),
                                                     themeRef.scheme().chartCpu,
                                                     std::nullopt,
                                                     2.0F,
                                                     true,
                                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);

                                    if (ImPlot::IsPlotHovered() && !timeData.empty())
                                    {
                                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                                        {
                                            ImGui::BeginTooltip();
                                            const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                                            ImGui::TextUnformatted(ageText.c_str());
                                            ImGui::Separator();
                                            if (*idxVal < sampleData.size())
                                            {
                                                ImGui::TextColored(
                                                    themeRef.scheme().chartCpu,
                                                    "CPU: %s",
                                                    UI::Format::percentCompact(static_cast<double>(sampleData[*idxVal])).c_str());
                                            }
                                            ImGui::EndTooltip();
                                        }
                                    }
                                }
                            };

                            const double smoothed = (ctx.smoothedPerCore != nullptr && coreIdx < ctx.smoothedPerCore->size())
                                                      ? (*ctx.smoothedPerCore)[coreIdx]
                                                      : snap.cpuPerCore[coreIdx].totalPercent;
                            const NowBar bar{.valueText = UI::Format::percentCompact(smoothed),
                                             .label = std::format("Core {}", coreIdx),
                                             .tooltipText = {},
                                             .value01 = UI::Format::percent01(smoothed),
                                             .color = theme.progressColor(smoothed)};

                            std::vector<NowBar> bars;
                            bars.push_back(bar);
                            // coreLabel is already allocated above for the visible label text, so
                            // reusing it here as the RenderMetrics/table id costs nothing extra --
                            // unlike the per-cell PushID scaffolding in ChartGrid.h, there's no
                            // allocation to avoid, and a per-core id keeps RenderMetrics entries
                            // from collapsing all cores into one (#823 review).
                            renderHistoryWithNowBars(coreLabel.c_str(), plotHeight, plotFn, bars, false, 0, true);
                        });
    }
}

void updateSmoothedPerCore(const Domain::SystemSnapshot& snap, RenderContext& ctx)
{
    if (ctx.smoothedPerCore == nullptr)
    {
        return;
    }

    using UI::Format::clampPercent;

    const double alpha = computeAlpha(ctx.lastDeltaSeconds, ctx.refreshInterval);
    const size_t numCores = snap.cpuPerCore.size();
    ctx.smoothedPerCore->resize(numCores, 0.0);

    for (size_t i = 0; i < numCores; ++i)
    {
        const double target = clampPercent(snap.cpuPerCore[i].totalPercent);
        double& current = (*ctx.smoothedPerCore)[i];
        if (ctx.lastDeltaSeconds <= 0.0F)
        {
            current = target;
            continue;
        }
        current = clampPercent(smoothTowards(current, target, alpha));
    }
}

} // namespace App::CpuCoresSection
