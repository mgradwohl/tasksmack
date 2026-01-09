#include "CpuCoresSection.h"

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"
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
#include <vector>

namespace App::CpuCoresSection
{

namespace
{

using UI::Widgets::BAR_WIDTH;
using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
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

void renderCpuCoresSection(RenderContext& ctx)
{
    if (ctx.systemModel == nullptr)
    {
        ImGui::TextUnformatted("System model not available");
        return;
    }

    auto snap = ctx.systemModel->snapshot();
    auto perCoreHist = ctx.systemModel->perCoreHistory();
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
    const auto& timestamps = (ctx.timestampsCache != nullptr) ? *ctx.timestampsCache : ctx.systemModel->timestamps();
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto axisConfig = makeTimeAxisConfig(timestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

    if (perCoreHist.empty() || timestamps.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "Collecting data...");
        return;
    }

    const size_t coreCount = perCoreHist.size();

    // Grid layout
    {
        const float gridWidth = ImGui::GetContentRegionAvail().x;
        constexpr float minCellWidth = 240.0F;
        const float barWidth = ImGui::GetFrameHeight(); // Match prior horizontal bar height for visual consistency
        const float cellWidth = minCellWidth + barWidth;
        const size_t gridCols = std::max<size_t>(1, static_cast<size_t>(gridWidth / cellWidth));
        const int gridColsInt = UI::Format::checkedCount(gridCols);
        const size_t gridRows = (coreCount + gridCols - 1) / gridCols;

        if (ImGui::BeginTable("PerCoreGrid", gridColsInt, ImGuiTableFlags_SizingStretchSame))
        {
            for (size_t row = 0; row < gridRows; ++row)
            {
                ImGui::TableNextRow();
                for (size_t col = 0; col < gridCols; ++col)
                {
                    const size_t coreIdx = (row * gridCols) + col;
                    ImGui::TableNextColumn();

                    if (coreIdx >= coreCount)
                    {
                        continue;
                    }

                    const auto& samples = perCoreHist[coreIdx];
                    if (samples.empty())
                    {
                        ImGui::TextColored(theme.scheme().textMuted, "Core %zu\nCollecting data...", coreIdx);
                        continue;
                    }

                    const std::string coreLabel = std::format(ICON_FA_MICROCHIP " Core {}", coreIdx);

                    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.scheme().childBg);
                    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().separator);
                    const std::string childId = std::format("CoreCell{}", coreIdx);
                    const float labelHeight = ImGui::GetTextLineHeight();
                    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
                    const float childHeight = labelHeight + spacingY + HISTORY_PLOT_HEIGHT_DEFAULT + BAR_WIDTH + (spacingY * 2.0F);
                    if (ImGui::BeginChild(childId.c_str(), ImVec2(-FLT_MIN, childHeight), ImGuiChildFlags_Borders))
                    {
                        const float availableWidth = ImGui::GetContentRegionAvail().x;
                        const float labelWidth = ImGui::CalcTextSize(coreLabel.c_str()).x;
                        const float labelOffset = std::max(0.0F, (availableWidth - labelWidth) * 0.5F);
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + labelOffset);
                        ImGui::TextUnformatted(coreLabel.c_str());
                        ImGui::Spacing();

                        std::vector<float> timeData = buildTimeAxis(timestamps, samples.size(), nowSeconds);

                        // Capture necessary variables by value/reference for lambda
                        const auto& sampleData = samples;
                        const auto& themeRef = theme;
                        const auto& axisCfg = axisConfig;

                        auto plotFn = [&timeData, &sampleData, &themeRef, &axisCfg]()
                        {
                            const UI::Widgets::PlotFontGuard fontGuard;
                            if (ImPlot::BeginPlot("##PerCorePlot", ImVec2(-1, HISTORY_PLOT_HEIGHT_DEFAULT), PLOT_FLAGS_DEFAULT))
                            {
                                ImPlot::SetupAxes("Time (s)", nullptr, X_AXIS_FLAGS_DEFAULT, ImPlotAxisFlags_Lock | Y_AXIS_FLAGS_DEFAULT);
                                ImPlot::SetupAxisFormat(ImAxis_Y1, UI::Widgets::formatAxisPercent);
                                ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImPlotCond_Always);
                                ImPlot::SetupAxisLimits(ImAxis_X1, axisCfg.xMin, axisCfg.xMax, ImPlotCond_Always);

                                plotLineWithFill("##Core",
                                                 timeData.data(),
                                                 sampleData.data(),
                                                 UI::Format::checkedCount(timeData.size()),
                                                 themeRef.scheme().chartCpu);

                                if (ImPlot::IsPlotHovered() && !timeData.empty())
                                {
                                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                                    {
                                        ImGui::BeginTooltip();
                                        const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                                        ImGui::TextUnformatted(ageText.c_str());
                                        if (*idxVal < sampleData.size())
                                        {
                                            ImGui::TextColored(
                                                themeRef.scheme().chartCpu, "CPU: %.1f%%", static_cast<double>(sampleData[*idxVal]));
                                        }
                                        ImGui::EndTooltip();
                                    }
                                }
                                ImPlot::EndPlot();
                            }
                        };

                        const double smoothed = (ctx.smoothedPerCore != nullptr && coreIdx < ctx.smoothedPerCore->size())
                                                  ? (*ctx.smoothedPerCore)[coreIdx]
                                                  : snap.cpuPerCore[coreIdx].totalPercent;
                        const NowBar bar{.valueText = UI::Format::percentCompact(smoothed),
                                         .label = std::format("Core {}", coreIdx),
                                         .value01 = UI::Format::percent01(smoothed),
                                         .color = theme.progressColor(smoothed)};

                        std::vector<NowBar> bars;
                        bars.push_back(bar);
                        const std::string tableId = std::format("CoreLayout{}", coreIdx);
                        renderHistoryWithNowBars(tableId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, plotFn, bars, false, 0, true);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor(2);
                }
            }
            ImGui::EndTable();
        }
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
