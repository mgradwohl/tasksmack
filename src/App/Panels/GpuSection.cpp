#include "GpuSection.h"

#include "Domain/GPUSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace App::GpuSection
{

namespace
{

using UI::Widgets::buildTimeAxis;
using UI::Widgets::computeAlpha;
using UI::Widgets::formatAgeSeconds;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::plotLineWithFill;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::tailAlignedSpan;

/// Scale each sample to a 0–100 percentage relative to maxVal, filling the output vector in-place.
/// Accepts a reusable buffer to avoid per-call heap allocation.
void normalizeToPercent(std::span<const float> hist, float maxVal, std::vector<float>& out)
{
    out.resize(hist.size());
    std::ranges::transform(hist, out.begin(), [maxVal](float v) { return (v / maxVal) * 100.0F; });
}

/// Render a "Note: This system does not report GPU X, Y or Z" line in muted text.
void renderUnavailableMetricsNote(std::span<const std::string> unavailable, ImVec4 textColor)
{
    if (unavailable.empty())
    {
        return;
    }
    std::string noteText = "Note: This system does not report GPU ";
    for (std::size_t i = 0; i < unavailable.size(); ++i)
    {
        if (i > 0)
        {
            noteText += (i == unavailable.size() - 1) ? " or " : ", ";
        }
        noteText += unavailable[i];
    }
    ImGui::TextColored(textColor, "%s", noteText.c_str());
}

} // namespace

void updateSmoothedGPU(const std::string& gpuId, const Domain::GPUSnapshot& snap, RenderContext& ctx)
{
    if (ctx.smoothedGPUs == nullptr)
    {
        return;
    }

    const double alpha = computeAlpha(ctx.lastDeltaSeconds, ctx.refreshInterval);

    auto& smoothed = (*ctx.smoothedGPUs)[gpuId];
    const bool initialized = smoothed.initialized;
    smoothed.utilizationPercent = initializeOrSmooth(smoothed.utilizationPercent, snap.utilizationPercent, alpha, initialized);
    smoothed.memoryPercent = initializeOrSmooth(smoothed.memoryPercent, snap.memoryUsedPercent, alpha, initialized);
    smoothed.temperatureC = initializeOrSmooth(smoothed.temperatureC, static_cast<double>(snap.temperatureC), alpha, initialized);
    smoothed.powerWatts = initializeOrSmooth(smoothed.powerWatts, snap.powerDrawWatts, alpha, initialized);
    smoothed.initialized = true;
}

void renderGpuSection(RenderContext& ctx)
{
    if (ctx.publication == nullptr)
    {
        ImGui::Text("GPU monitoring not available");
        return;
    }

    const auto& gpuSnapshots = ctx.publication->snapshots;
    const auto& gpuInfos = ctx.publication->gpuInfo;
    const auto& caps = ctx.publication->capabilities;
    auto& theme = UI::Theme::get();

    if (gpuSnapshots.empty())
    {
        ImGui::TextColored(theme.scheme().textMuted, "No GPU data available");
        return;
    }

    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();

    ImGui::Text("GPU Monitoring (%zu GPU%s)", gpuSnapshots.size(), gpuSnapshots.size() == 1 ? "" : "s");
    ImGui::Spacing();

    // Update smoothed values for all GPUs
    for (const auto& snap : gpuSnapshots)
    {
        updateSmoothedGPU(snap.gpuId, snap, ctx);
    }

    // Scratch buffers for normalizeToPercent — declared before the GPU loop so they are reused
    // across multiple GPU iterations in the same frame (resize only allocates when count grows).
    std::vector<float> clockPercentBuf;
    std::vector<float> tempPercentBuf;
    std::vector<float> powerPercentBuf;

    // Render each GPU
    for (size_t gpuIdx = 0; gpuIdx < gpuSnapshots.size(); ++gpuIdx)
    {
        const auto& snap = gpuSnapshots[gpuIdx];
        const auto& smoothed = (*ctx.smoothedGPUs)[snap.gpuId];

        // Find GPU info for this GPU
        std::string gpuName = snap.name;
        bool isIntegrated = snap.isIntegrated;
        for (const auto& info : gpuInfos)
        {
            if (info.id == snap.gpuId)
            {
                gpuName = info.name;
                isIntegrated = info.isIntegrated;
                break;
            }
        }

        // GPU header with collapsible section
        // Discrete: show VRAM amount after name, label as "Discrete"
        // Integrated: no VRAM amount (shares system RAM), label as "Shared Memory"
        std::string vramInfo;
        if (!isIntegrated && snap.memoryTotalBytes > 0)
        {
            vramInfo = std::format(", {} VRAM", UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes)));
        }
        const std::string headerLabel =
            std::format("{} {}{} [{}]", ICON_FA_MICROCHIP, gpuName, vramInfo, isIntegrated ? "Shared Memory" : "Discrete");

        ImGui::PushID(static_cast<int>(gpuIdx)); // gpuIdx is a small index; explicit narrowing to match ImGui API
        const bool expanded = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopID();

        if (!expanded)
        {
            continue;
        }

        ImGui::Indent();

        // Get history data for this GPU
        const auto historyIt = ctx.publication->histories.find(snap.gpuId);
        if (historyIt == ctx.publication->histories.end())
        {
            ImGui::Unindent();
            continue;
        }
        const auto& history = historyIt->second;
        const auto& utilHist = history.utilization;
        const auto& memHist = history.memoryPercent;
        const auto& clockHist = history.gpuClock;
        const auto& encoderHist = history.encoder;
        const auto& decoderHist = history.decoder;
        const auto& tempHist = history.temperature;
        const auto& powerHist = history.power;
        const auto& fanHist = history.fanSpeed;

        // Per-GPU timestamps: only includes samples when this GPU was present,
        // so they stay aligned with the per-GPU history vectors even when the GPU
        // was intermittently absent (global timestamps include samples this GPU never recorded).
        const auto& perGpuTimestamps = history.timestamps;

        const size_t alignedCount = std::min({utilHist.size(), memHist.size(), perGpuTimestamps.size()});
        const auto utilData = tailAlignedSpan(utilHist, alignedCount).values;
        const auto memData = tailAlignedSpan(memHist, alignedCount).values;
        const auto clockData = tailAlignedSpan(clockHist, alignedCount).values;
        const auto encoderData = tailAlignedSpan(encoderHist, alignedCount).values;
        const auto decoderData = tailAlignedSpan(decoderHist, alignedCount).values;
        const auto tempData = tailAlignedSpan(tempHist, alignedCount).values;
        const auto powerData = tailAlignedSpan(powerHist, alignedCount).values;
        const auto fanData = tailAlignedSpan(fanHist, alignedCount).values;
        const auto snapshotData = tailAlignedSpan(history.snapshots, alignedCount).values;

        std::vector<float> timeData = buildTimeAxis(perGpuTimestamps, alignedCount, nowSeconds);

        // Compute per-GPU axis config from per-GPU timestamps so that X-axis scroll/limits
        // stay consistent with the data being plotted even when a GPU is intermittently absent
        // (global timestamps would include samples this GPU never recorded, causing a mismatch).
        const auto axisConfig = makeTimeAxisConfig(perGpuTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

        // Get max clock for normalization
        const float maxClockMHz =
            caps.hasClockSpeeds && snap.gpuClockMHz > 0 ? static_cast<float>(std::max(snap.gpuClockMHz, 2000U)) : 2000.0F;

        // ========================================
        // Chart 1: Core + Video (all percentages)
        // Utilization, Memory, Clock, Encoder, Decoder
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_VIDEO "  GPU Core & Video (%zu samples)", alignedCount);

        auto gpuCorePlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::percentHistoryConfig("##GPUCoreHistory", axisConfig.xMin, axisConfig.xMax));
            if (chart.active())
            {
                if (!utilData.empty())
                {
                    plotLineWithFill("Utilization",
                                     timeData.data(),
                                     utilData.data(),
                                     UI::Format::checkedCount(utilData.size()),
                                     theme.scheme().gpuUtilization,
                                     std::nullopt,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                }

                if (!memData.empty())
                {
                    plotLineWithFill("Memory",
                                     timeData.data(),
                                     memData.data(),
                                     UI::Format::checkedCount(memData.size()),
                                     theme.scheme().gpuMemory,
                                     std::nullopt,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                }

                // Plot clock as normalized percentage (0-maxClockMHz mapped to 0-100)
                if (caps.hasClockSpeeds && !clockData.empty())
                {
                    normalizeToPercent(clockData, maxClockMHz, clockPercentBuf);
                    const auto clockTimeData = tailAlignedSpan(timeData, clockPercentBuf.size());
                    const auto clockLabel = std::format("Clock (% of {:.0f} MHz)", static_cast<double>(maxClockMHz));
                    plotLineWithFill(clockLabel.c_str(),
                                     clockTimeData.values.data(),
                                     clockPercentBuf.data(),
                                     UI::Format::checkedCount(clockTimeData.values.size()),
                                     theme.scheme().gpuClock,
                                     std::nullopt,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                }

                // Encoder utilization
                if (caps.hasEncoderDecoder && !encoderData.empty())
                {
                    const auto encoderTimeData = tailAlignedSpan(timeData, encoderData.size());
                    plotLineWithFill("Encoder",
                                     encoderTimeData.values.data(),
                                     encoderData.data(),
                                     UI::Format::checkedCount(encoderTimeData.values.size()),
                                     theme.scheme().gpuEncoder,
                                     std::nullopt,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                }

                // Decoder utilization
                if (caps.hasEncoderDecoder && !decoderData.empty())
                {
                    const auto decoderTimeData = tailAlignedSpan(timeData, decoderData.size());
                    plotLineWithFill("Decoder",
                                     decoderTimeData.values.data(),
                                     decoderData.data(),
                                     UI::Format::checkedCount(decoderTimeData.values.size()),
                                     theme.scheme().gpuDecoder,
                                     std::nullopt,
                                     2.0F,
                                     true,
                                     UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                }

                // Tooltip on hover
                if (ImPlot::IsPlotHovered() && !timeData.empty())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        // Fetch only the single snapshot needed for the hovered index.
                        // snapshotAt() avoids copying the full history vector (unlike GPUModel::history()).
                        // perGpuTimestamps and the GPU history are always the same length and aligned
                        // sample-for-sample, so *idxVal maps directly to the correct history entry.
                        const auto* histSnap = *idxVal < snapshotData.size() ? &snapshotData[*idxVal] : nullptr;

                        ImGui::BeginTooltip();
                        const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                        ImGui::TextUnformatted(ageText.c_str());
                        ImGui::Separator();
                        if (*idxVal < utilData.size())
                        {
                            ImGui::TextColored(
                                theme.scheme().gpuUtilization, "Utilization: %s", UI::Format::percentCompact(utilData[*idxVal]).c_str());
                        }
                        if (*idxVal < memData.size())
                        {
                            const auto pct = static_cast<double>(memData[*idxVal]);
                            if (histSnap != nullptr && histSnap->memoryTotalBytes > 0)
                            {
                                ImGui::TextColored(
                                    theme.scheme().gpuMemory,
                                    "Memory: %s",
                                    UI::Format::bytesUsedTotalPercentCompact(histSnap->memoryUsedBytes, histSnap->memoryTotalBytes, pct)
                                        .c_str());
                            }
                            else
                            {
                                ImGui::TextColored(theme.scheme().gpuMemory, "Memory: %s", UI::Format::percentCompact(pct).c_str());
                            }
                        }
                        if (caps.hasClockSpeeds && !clockData.empty())
                        {
                            const auto clockTimeData = tailAlignedSpan(timeData, clockData.size());
                            if (*idxVal >= clockTimeData.offset)
                            {
                                const size_t clockIdx = *idxVal - clockTimeData.offset;
                                ImGui::TextColored(
                                    theme.scheme().gpuClock, "Clock: %u MHz", static_cast<unsigned int>(clockData[clockIdx]));
                            }
                        }
                        if (caps.hasEncoderDecoder && !encoderData.empty())
                        {
                            const auto encoderTimeData = tailAlignedSpan(timeData, encoderData.size());
                            if (*idxVal >= encoderTimeData.offset)
                            {
                                const size_t encoderIdx = *idxVal - encoderTimeData.offset;
                                ImGui::TextColored(
                                    theme.scheme().gpuEncoder, "Encoder: %s", UI::Format::percentCompact(encoderData[encoderIdx]).c_str());
                            }
                        }
                        if (caps.hasEncoderDecoder && !decoderData.empty())
                        {
                            const auto decoderTimeData = tailAlignedSpan(timeData, decoderData.size());
                            if (*idxVal >= decoderTimeData.offset)
                            {
                                const size_t decoderIdx = *idxVal - decoderTimeData.offset;
                                ImGui::TextColored(
                                    theme.scheme().gpuDecoder, "Decoder: %s", UI::Format::percentCompact(decoderData[decoderIdx]).c_str());
                            }
                        }
                        ImGui::EndTooltip();
                    }
                }
            }
        };

        // Build now bars for chart 1: utilization, memory, clock, encoder, decoder
        std::vector<NowBar> gpuCoreBars;
        gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(smoothed.utilizationPercent),
                               .label = "GPU Utilization",
                               .tooltipText = {},
                               .value01 = UI::Format::percent01(smoothed.utilizationPercent),
                               .color = theme.scheme().gpuUtilization});
        if (snap.memoryTotalBytes > 0)
        {
            // Use the snapshot's own computed percent and raw byte values so the percent
            // and byte figures always come from the same sample and cannot show an
            // impossible combination (e.g. 50% with 8 GiB / 8 GiB).
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(smoothed.memoryPercent),
                                   .label = "GPU Memory",
                                   .tooltipText = std::format("GPU Memory: {} ({} / {})",
                                                              UI::Format::percentCompact(snap.memoryUsedPercent),
                                                              UI::Format::formatBytes(static_cast<double>(snap.memoryUsedBytes)),
                                                              UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes))),
                                   .value01 = UI::Format::percent01(smoothed.memoryPercent),
                                   .color = theme.scheme().gpuMemory});
        }
        else
        {
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(smoothed.memoryPercent),
                                   .label = "GPU Memory",
                                   .tooltipText = std::format("GPU Memory: {}", UI::Format::percentCompact(smoothed.memoryPercent)),
                                   .value01 = UI::Format::percent01(smoothed.memoryPercent),
                                   .color = theme.scheme().gpuMemory});
        }
        if (caps.hasClockSpeeds && snap.gpuClockMHz > 0)
        {
            const double clockPercent = (static_cast<double>(snap.gpuClockMHz) / static_cast<double>(maxClockMHz)) * 100.0;
            gpuCoreBars.push_back({.valueText = std::format("{} MHz", snap.gpuClockMHz),
                                   .label = "GPU Clock",
                                   .tooltipText = {},
                                   .value01 = UI::Format::percent01(clockPercent),
                                   .color = theme.scheme().gpuClock});
        }
        if (caps.hasEncoderDecoder)
        {
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(snap.encoderUtilPercent),
                                   .label = "Encoder",
                                   .tooltipText = {},
                                   .value01 = UI::Format::percent01(snap.encoderUtilPercent),
                                   .color = theme.scheme().gpuEncoder});
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(snap.decoderUtilPercent),
                                   .label = "Decoder",
                                   .tooltipText = {},
                                   .value01 = UI::Format::percent01(snap.decoderUtilPercent),
                                   .color = theme.scheme().gpuDecoder});
        }

        // Build thermal bars early so we can calculate max column count for alignment
        std::vector<NowBar> gpuThermalBars;
        constexpr float maxTempC = 100.0F;
        const float maxPowerW = snap.powerLimitWatts > 0.0 ? static_cast<float>(snap.powerLimitWatts) : 300.0F;
        if (caps.hasTemperature)
        {
            const double tempPercent = (smoothed.temperatureC / static_cast<double>(maxTempC)) * 100.0;
            gpuThermalBars.push_back({.valueText = std::format("{}°C", static_cast<int>(smoothed.temperatureC)),
                                      .label = "GPU Temperature",
                                      .tooltipText = {},
                                      .value01 = UI::Format::percent01(tempPercent),
                                      .color = theme.scheme().gpuTemperature});
        }
        if (caps.hasPowerMetrics)
        {
            const double powerPercent = (smoothed.powerWatts / static_cast<double>(maxPowerW)) * 100.0;
            gpuThermalBars.push_back({.valueText = std::format("{:.1f}W", smoothed.powerWatts),
                                      .label = "GPU Power",
                                      .tooltipText = std::format("GPU Power: {:.2Lf} W", smoothed.powerWatts),
                                      .value01 = UI::Format::percent01(powerPercent),
                                      .color = theme.scheme().gpuPower});
        }
        // Keep pushing a bar (stable column count) whenever the capability is present, so the
        // now-bar layout doesn't jitter frame-to-frame as fanSpeedAvailable flips on a transient
        // per-poll read failure - but show "N/A" instead of a misleading "0%" for a sample that
        // failed to read the sensor (see fanSpeedAvailable's comment in GPUSnapshot.h).
        if (caps.hasFanSpeed)
        {
            NowBar fanBar{.valueText = {}, .label = "GPU Fan Speed", .tooltipText = {}, .value01 = 0.0, .color = {}};
            if (snap.fanSpeedAvailable)
            {
                fanBar.valueText = std::format("{}%", snap.fanSpeedPercent);
                fanBar.value01 = UI::Format::percent01(static_cast<double>(snap.fanSpeedPercent));
                fanBar.color = theme.scheme().gpuFan;
            }
            else
            {
                fanBar.valueText = "N/A";
                fanBar.tooltipText = "GPU Fan Speed: unavailable this sample";
                fanBar.color = theme.scheme().textMuted;
            }
            gpuThermalBars.push_back(fanBar);
        }

        // Use max bar count across both charts for x-axis alignment
        const size_t gpuNowBarColumns = std::max(gpuCoreBars.size(), gpuThermalBars.size());

        const std::string coreLayoutId = std::format("GPUCoreLayout{}", gpuIdx);
        renderHistoryWithNowBars(coreLayoutId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, gpuCorePlot, gpuCoreBars, false, gpuNowBarColumns);

        // Show notes for unavailable core metrics
        {
            std::vector<std::string> unavailableCoreNotes;
            if (!caps.hasClockSpeeds)
            {
                unavailableCoreNotes.emplace_back("clock speed");
            }
            if (!caps.hasEncoderDecoder)
            {
                unavailableCoreNotes.emplace_back("encoder/decoder utilization");
            }

            if (!unavailableCoreNotes.empty())
            {
                renderUnavailableMetricsNote(unavailableCoreNotes, theme.scheme().textMuted);
            }
        }

        ImGui::Spacing();

        // ========================================
        // Chart 2: Thermal/Power (temp, power, fan)
        // These have different units, normalize to percentage for display
        // ========================================
        if (caps.hasTemperature || caps.hasPowerMetrics || caps.hasFanSpeed)
        {
            ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_TEMPERATURE_HALF "  Thermal & Power");

            // Note: maxTempC and maxPowerW are defined above with the thermal bars
            // Note: Fan speed is already a percentage, no max needed for normalization - but
            // unlike temp/power it isn't clamped to 100 (see GPUModel::computeSnapshot), so a
            // reading above the chart's locked 0-100 range renders clipped at the top; an
            // unavailable sample is NaN (a gap in the line), not a misleading 0%.

            auto gpuThermalPlot = [&]()
            {
                const UI::Widgets::HistoryChart chart(
                    UI::Widgets::percentHistoryConfig("##GPUThermalHistory", axisConfig.xMin, axisConfig.xMax));
                if (chart.active())
                {
                    // Temperature (normalized to 0-100%)
                    if (caps.hasTemperature && !tempData.empty())
                    {
                        normalizeToPercent(tempData, maxTempC, tempPercentBuf);
                        const auto tempTimeData = tailAlignedSpan(timeData, tempPercentBuf.size());
                        plotLineWithFill("Temp (% of 100°C)",
                                         tempTimeData.values.data(),
                                         tempPercentBuf.data(),
                                         UI::Format::checkedCount(tempTimeData.values.size()),
                                         theme.scheme().gpuTemperature,
                                         std::nullopt,
                                         2.0F,
                                         true,
                                         UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                    }

                    // Power (normalized to actual reference watts; includes fallback note when limit is unavailable)
                    if (caps.hasPowerMetrics && !powerData.empty())
                    {
                        normalizeToPercent(powerData, maxPowerW, powerPercentBuf);
                        const auto powerTimeData = tailAlignedSpan(timeData, powerPercentBuf.size());
                        const bool hasPowerLimitReference = snap.powerLimitWatts > 0.0;
                        const auto powerLabel = std::format(
                            "Power (% of {:.0f}W{})", static_cast<double>(maxPowerW), hasPowerLimitReference ? "" : " fallback");
                        plotLineWithFill(powerLabel.c_str(),
                                         powerTimeData.values.data(),
                                         powerPercentBuf.data(),
                                         UI::Format::checkedCount(powerTimeData.values.size()),
                                         theme.scheme().gpuPower,
                                         std::nullopt,
                                         2.0F,
                                         true,
                                         UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                    }

                    // Fan speed (already a percentage)
                    if (caps.hasFanSpeed && !fanData.empty())
                    {
                        const auto fanTimeData = tailAlignedSpan(timeData, fanData.size());
                        plotLineWithFill("Fan",
                                         fanTimeData.values.data(),
                                         fanData.data(),
                                         UI::Format::checkedCount(fanTimeData.values.size()),
                                         theme.scheme().gpuFan,
                                         std::nullopt,
                                         2.0F,
                                         true,
                                         UI::Widgets::LINE_PLOT_MAX_POINTS_DENSE);
                    }

                    // Tooltip on hover
                    if (ImPlot::IsPlotHovered() && !timeData.empty())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            ImGui::BeginTooltip();
                            const auto ageText = formatAgeSeconds(static_cast<double>(timeData[*idxVal]));
                            ImGui::TextUnformatted(ageText.c_str());
                            ImGui::Separator();
                            if (caps.hasTemperature && !tempData.empty())
                            {
                                const auto tempTimeData = tailAlignedSpan(timeData, tempData.size());
                                if (*idxVal >= tempTimeData.offset)
                                {
                                    const size_t tempIdx = *idxVal - tempTimeData.offset;
                                    ImGui::TextColored(
                                        theme.scheme().gpuTemperature, "Temperature: %d°C", static_cast<int>(tempData[tempIdx]));
                                }
                            }
                            if (caps.hasPowerMetrics && !powerData.empty())
                            {
                                const auto powerTimeData = tailAlignedSpan(timeData, powerData.size());
                                if (*idxVal >= powerTimeData.offset)
                                {
                                    const size_t powerIdx = *idxVal - powerTimeData.offset;
                                    ImGui::TextColored(theme.scheme().gpuPower, "Power: %.1fW", static_cast<double>(powerData[powerIdx]));
                                }
                            }
                            if (caps.hasFanSpeed && !fanData.empty())
                            {
                                const auto fanTimeData = tailAlignedSpan(timeData, fanData.size());
                                if (*idxVal >= fanTimeData.offset)
                                {
                                    const size_t fanIdx = *idxVal - fanTimeData.offset;
                                    // NaN marks a sample where the fan couldn't be read (see the
                                    // GPUModel::publish() comment) - show that as unavailable
                                    // rather than casting NaN to an unsigned int.
                                    if (std::isnan(fanData[fanIdx]))
                                    {
                                        ImGui::TextColored(theme.scheme().textMuted, "Fan: N/A");
                                    }
                                    else
                                    {
                                        // Format the float directly rather than narrowing to unsigned int: this PR
                                        // intentionally leaves fanSpeedPercent unclamped (see GPUModel::computeSnapshot),
                                        // so a corrupted-input value can exceed unsigned int's range, and casting an
                                        // out-of-range float to an integer type is undefined behavior.
                                        ImGui::TextColored(theme.scheme().gpuFan, "Fan: %.0f%%", static_cast<double>(fanData[fanIdx]));
                                    }
                                }
                            }
                            ImGui::EndTooltip();
                        }
                    }
                }
            };

            // Thermal bars were already built above for alignment calculation
            // Render thermal chart with the same column count as core chart for x-axis alignment
            if (!gpuThermalBars.empty())
            {
                const std::string thermalLayoutId = std::format("GPUThermalLayout{}", gpuIdx);
                renderHistoryWithNowBars(
                    thermalLayoutId.c_str(), HISTORY_PLOT_HEIGHT_DEFAULT, gpuThermalPlot, gpuThermalBars, false, gpuNowBarColumns);
            }
            else
            {
                // No current data, just render the plot without now bars
                gpuThermalPlot();
            }

            // Show notes for unavailable metrics
            std::vector<std::string> unavailableNotes;
            if (!caps.hasTemperature)
            {
                unavailableNotes.emplace_back("temperature");
            }
            if (!caps.hasPowerMetrics)
            {
                unavailableNotes.emplace_back("power draw");
            }
            if (!caps.hasFanSpeed)
            {
                unavailableNotes.emplace_back("fan speed");
            }

            if (!unavailableNotes.empty())
            {
                renderUnavailableMetricsNote(unavailableNotes, theme.scheme().textMuted);
            }
        }

        ImGui::Unindent();
        ImGui::Spacing();
    }
}

} // namespace App::GpuSection
