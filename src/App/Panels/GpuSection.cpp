#include "GpuSection.h"

#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"
#include "UI/ChartWidgets.h"
#include "UI/EmptyState.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
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
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace App::GpuSection
{

namespace
{

using UI::Widgets::computeAlpha;
using UI::Widgets::frameTimeAxis;
using UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
using UI::Widgets::hoveredIndexFromPlotX;
using UI::Widgets::initializeOrSmooth;
using UI::Widgets::makeTimeAxisConfig;
using UI::Widgets::NowBar;
using UI::Widgets::NowBarList;
using UI::Widgets::plotSeries;
using UI::Widgets::renderHistoryWithNowBars;
using UI::Widgets::SeriesRole;
using UI::Widgets::seriesStyle;
using UI::Widgets::tailAlignedSpan;

// One label per series, shared by its value-strip entry, tooltip row and NowBar (#1008). The bars and
// tooltips used to say "GPU Utilization", "GPU Temperature", "GPU Fan Speed" for series the legend
// called "Utilization", "Temp (% of 100°C)" and "Fan". The scale a normalised series is drawn
// against belongs in its value ("65°C (65% of 100°C)"), not its name, which stays fixed (#994).
constexpr const char* UTIL_LABEL = "Utilization";
constexpr const char* MEMORY_LABEL = "Memory";
constexpr const char* CLOCK_LABEL = "Clock";
constexpr const char* ENCODER_LABEL = "Encoder";
constexpr const char* DECODER_LABEL = "Decoder";
constexpr const char* TEMP_LABEL = "Temperature";
constexpr const char* POWER_LABEL = "Power";
constexpr const char* FAN_LABEL = "Fan";

/// Pushes one GPU's ImGui ID for the scope, popping it on every path out, early continues
/// included. Keyed by the GPU's id rather than its position, so a GPU missing from a read does not
/// hand its header and chart state to the GPU after it (#1163).
class ScopedGpuId
{
  public:
    explicit ScopedGpuId(std::string_view gpuId)
    {
        ImGui::PushID(gpuId.data(), gpuId.data() + gpuId.size());
    }
    ~ScopedGpuId()
    {
        ImGui::PopID();
    }
    ScopedGpuId(const ScopedGpuId&) = delete;
    ScopedGpuId& operator=(const ScopedGpuId&) = delete;
    ScopedGpuId(ScopedGpuId&&) = delete;
    ScopedGpuId& operator=(ScopedGpuId&&) = delete;
};

/// Scale each sample to a 0–100 percentage relative to maxVal, filling the output vector in-place.
/// Accepts a reusable buffer to avoid per-call heap allocation.
void normalizeToPercent(std::span<const float> hist, float maxVal, std::vector<float>& out)
{
    out.resize(hist.size());
    std::ranges::transform(hist, out.begin(), [maxVal](float v) { return (v / maxVal) * 100.0F; });
}

/// The metrics a GPU does not report, named for renderUnavailableMetricsNote(). Held in place: the
/// names are constants, and the list is built every frame (#1171).
class UnavailableMetrics
{
  public:
    void add(std::string_view metric) noexcept
    {
        if (m_Count < m_Names.size())
        {
            m_Names[m_Count++] = metric;
        }
    }

    [[nodiscard]] std::span<const std::string_view> names() const noexcept
    {
        return {m_Names.data(), m_Count};
    }

  private:
    std::array<std::string_view, 3> m_Names{};
    std::size_t m_Count = 0;
};

/// Render a "Note: This system does not report GPU X, Y or Z" line in muted text. Composed in a stack
/// buffer rather than a std::string, since it is drawn every frame (#1171).
void renderUnavailableMetricsNote(std::span<const std::string_view> unavailable, ImVec4 textColor)
{
    if (unavailable.empty())
    {
        return;
    }
    std::array<char, 160> noteText{};
    auto* out =
        std::format_to_n(noteText.data(), static_cast<std::ptrdiff_t>(noteText.size() - 1), "Note: This system does not report GPU ").out;
    for (std::size_t i = 0; i < unavailable.size(); ++i)
    {
        const std::ptrdiff_t room = static_cast<std::ptrdiff_t>(noteText.size() - 1) - (out - noteText.data());
        std::string_view separator;
        if (i > 0)
        {
            separator = (i == unavailable.size() - 1) ? " or " : ", ";
        }
        out = std::format_to_n(out, room, "{}{}", separator, unavailable[i]).out;
    }
    ImGui::TextColored(textColor, "%s", noteText.data());
}

/// Rebuilds @p cache's per-entry strings when @p publication is not the one they were built from.
void refreshEntryLabels(FrameCache& cache, const Domain::GPUPublication& publication)
{
    // All three per-entry vectors must be complete: an exception part-way through a rebuild (caught by
    // the render loop) can leave them different lengths, and they are indexed per entry below.
    if (cache.labelsPublication == &publication && cache.labelsVersion == publication.version &&
        cache.headerLabels.size() == cache.drawList.size() && cache.coreLayoutIds.size() == cache.drawList.size() &&
        cache.thermalLayoutIds.size() == cache.drawList.size())
    {
        return;
    }
    cache.labelsPublication = &publication;
    cache.labelsVersion = publication.version;
    cache.headerLabels.clear();
    cache.coreLayoutIds.clear();
    cache.thermalLayoutIds.clear();
    for (const GpuDrawEntry& entry : cache.drawList)
    {
        if (entry.snapshot == nullptr)
        {
            // Enumerated, but this read returned nothing for it. Same ###gpuHeader id as a GPU with a
            // reading, so its collapse state survives the gap.
            cache.headerLabels.push_back(gpuHeaderLabel(ICON_FA_MICROCHIP, entry.info->name, entry.info->isIntegrated, 0, false));
        }
        else
        {
            const auto& snap = *entry.snapshot;
            const std::string_view name = (entry.info != nullptr) ? std::string_view{entry.info->name} : std::string_view{snap.name};
            const bool isIntegrated = (entry.info != nullptr) ? entry.info->isIntegrated : snap.isIntegrated;
            // Discrete: VRAM amount after the name, labelled "Discrete". Integrated: no VRAM amount
            // (it shares system RAM), labelled "Shared Memory". A GPU the probe is leaving asleep is
            // labelled so (#1117).
            cache.headerLabels.push_back(gpuHeaderLabel(ICON_FA_MICROCHIP, name, isIntegrated, snap.memoryTotalBytes, snap.suspended));
        }
        cache.coreLayoutIds.push_back(std::format("GPUCoreLayout{}", entry.gpuId));
        cache.thermalLayoutIds.push_back(std::format("GPUThermalLayout{}", entry.gpuId));
    }
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
    // A field this sample couldn't read keeps its last value and is marked uninitialized (its bar
    // shows N/A); the next reading starts afresh rather than easing from it (#1111).
    const auto smoothReading = [alpha](double& value, bool& valueInitialized, bool available, double reading)
    {
        if (available)
        {
            value = initializeOrSmooth(value, reading, alpha, valueInitialized);
        }
        valueInitialized = available;
    };
    smoothReading(smoothed.utilizationPercent, smoothed.utilizationInitialized, snap.utilizationAvailable, snap.utilizationPercent);
    smoothReading(smoothed.memoryPercent, smoothed.memoryInitialized, snap.memoryAvailable, snap.memoryUsedPercent);
    smoothReading(
        smoothed.temperatureC, smoothed.temperatureInitialized, snap.temperatureAvailable, static_cast<double>(snap.temperatureC));
    smoothReading(smoothed.powerWatts, smoothed.powerInitialized, snap.powerAvailable, snap.powerDrawWatts);
    smoothed.encoderPercent = initializeOrSmooth(smoothed.encoderPercent, snap.encoderUtilPercent, alpha, initialized);
    smoothed.decoderPercent = initializeOrSmooth(smoothed.decoderPercent, snap.decoderUtilPercent, alpha, initialized);
    if (snap.gpuClockAvailable && snap.gpuClockMHz > 0)
    {
        smoothed.clockMHz = initializeOrSmooth(smoothed.clockMHz, static_cast<double>(snap.gpuClockMHz), alpha, smoothed.clockInitialized);
        smoothed.clockInitialized = true;
    }
    else
    {
        smoothed.clockInitialized = false;
    }
    if (snap.fanSpeedAvailable)
    {
        smoothed.fanPercent =
            initializeOrSmooth(smoothed.fanPercent, static_cast<double>(snap.fanSpeedPercent), alpha, smoothed.fanInitialized);
        smoothed.fanInitialized = true;
    }
    else
    {
        smoothed.fanInitialized = false;
    }
    smoothed.initialized = true;
}

void renderGpuSection(RenderContext& ctx)
{
    const EmptyReason emptyReason = classifyEmptyState(ctx.publication != nullptr,
                                                       (ctx.publication != nullptr) && ctx.publication->gpuInfoKnown,
                                                       (ctx.publication != nullptr) ? ctx.publication->gpuInfo.size() : 0,
                                                       (ctx.publication != nullptr) ? ctx.publication->snapshots.size() : 0);
    switch (emptyReason)
    {
    case EmptyReason::Unavailable:
        UI::Widgets::renderEmptyState(ICON_FA_TRIANGLE_EXCLAMATION "  GPU monitoring is not available",
                                      "TaskSmack could not read GPU data on this system.");
        return;
    case EmptyReason::NoDevices:
        // Not an error and not transient: the probe ran and reported no device. Saying so, and that
        // it is expected where it usually happens, is what distinguishes this from a failed tab.
        UI::Widgets::renderEmptyState(ICON_FA_MICROCHIP "  No GPU detected",
                                      "No GPU device was found. This is expected in most virtual machines and under WSL2, "
                                      "where no GPU device is exposed to the system.");
        return;
    case EmptyReason::NoReadings:
    {
        // Every known GPU missed this read. Say so, but keep drawing the list below: each GPU keeps
        // its slot and collapse state with "No reading" rather than the whole tab collapsing into an
        // empty state for one missed sample (#1163).
        const std::size_t deviceCount = ctx.publication->gpuInfo.size();
        std::array<char, 96> detail{};
        std::format_to_n(detail.data(),
                         static_cast<std::ptrdiff_t>(detail.size() - 1),
                         "{} GPU{} detected, but the latest reading returned no data.",
                         deviceCount,
                         deviceCount == 1 ? " was" : "s were");
        ImGui::TextDisabled("%s", detail.data());
        break;
    }
    case EmptyReason::None:
        break;
    }

    const auto& gpuSnapshots = ctx.publication->snapshots;
    // Kept by the panel across frames, so the draw list, scratch buffers and labels are reused (#1171).
    FrameCache frameLocalCache;
    FrameCache& cache = (ctx.cache != nullptr) ? *ctx.cache : frameLocalCache;
    // Enumeration order, each GPU looked up by id, so a GPU missing from this read keeps its slot (#1163).
    // Rebuilt every frame, into the cache's storage: its entries point into this publication.
    gpuDrawList(*ctx.publication, cache.drawList);
    const std::vector<GpuDrawEntry>& drawList = cache.drawList;
    refreshEntryLabels(cache, *ctx.publication);
    const auto& probeCaps = ctx.publication->capabilities;
    auto& theme = UI::Theme::get();

    const double nowSeconds = UI::Widgets::historyFrameNowSeconds(); // Shared with plotLineWithFill (see it)

    ImGui::Text("GPU Monitoring (%zu GPU%s)", drawList.size(), drawList.size() == 1 ? "" : "s");
    ImGui::Spacing();

    // Update smoothed values for all GPUs
    for (const auto& snap : gpuSnapshots)
    {
        updateSmoothedGPU(snap.gpuId, snap, ctx);
    }

    // Every chart on the tab, across all GPUs, gets the same share of its height.
    const float plotHeight = (ctx.fill != nullptr) ? ctx.fill->plotHeight() : HISTORY_PLOT_HEIGHT_DEFAULT;
    const auto countPlot = [&ctx]
    {
        if (ctx.fill != nullptr)
        {
            ctx.fill->addPlot();
        }
    };

    // Scratch buffers for normalizeToPercent, kept across GPUs and frames (resize only allocates when
    // the count grows).
    std::vector<float>& clockPercentBuf = cache.clockPercent;
    std::vector<float>& tempPercentBuf = cache.temperaturePercent;
    std::vector<float>& powerPercentBuf = cache.powerPercent;

    // Render each GPU
    for (std::size_t entryIndex = 0; entryIndex < drawList.size(); ++entryIndex)
    {
        const GpuDrawEntry& entry = drawList[entryIndex];
        // The GPU's whole body, header through charts, is under its own ID (#1163).
        const ScopedGpuId gpuIdScope(entry.gpuId);
        // Built once per publication (refreshEntryLabels()).
        const std::string& headerLabel = cache.headerLabels[entryIndex];

        if (entry.snapshot == nullptr)
        {
            // Enumerated, but this read returned nothing for it: keep its slot rather than drop it.
            if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Indent();
                ImGui::TextColored(theme.scheme().textMuted, "No reading");
                ImGui::Unindent();
                ImGui::Spacing();
            }
            continue;
        }

        const auto& snap = *entry.snapshot;
        const auto& smoothed = (*ctx.smoothedGPUs)[snap.gpuId];

        std::optional<Platform::GPUCapabilities> adapterSensors;
        if (entry.info != nullptr)
        {
            adapterSensors = entry.info->sensorCapabilities;
        }
        // What this GPU reports, not what the probe can report for some GPU (#1040).
        const Platform::GPUCapabilities caps = capabilitiesForGpu(probeCaps, adapterSensors);

        // GPU header with collapsible section; its label is built in refreshEntryLabels().
        // Scoped by the adapter's stable id (gpuIdScope above), not its position. The label's
        // ###gpuHeader suffix keeps the id fixed as the label changes ("(Sleeping)", VRAM).
        const bool expanded = ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

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

        // Per-GPU timestamps: one per refresh since this GPU was first seen, aligned with the
        // per-GPU history vectors. A refresh it was missing from has an entry too, whose values
        // are NaN, so the absence is drawn as a gap (#1146).
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
        const auto memUsedBytesData = tailAlignedSpan(history.memoryUsedBytes, alignedCount).values;
        const auto memTotalBytesData = tailAlignedSpan(history.memoryTotalBytes, alignedCount).values;

        const auto timeData = frameTimeAxis(perGpuTimestamps, alignedCount, nowSeconds);

        // Compute per-GPU axis config from per-GPU timestamps so that X-axis scroll/limits stay
        // consistent with the data being plotted. A refresh the GPU was missing from has a (gap)
        // entry of its own, but the GPU's history can still start later than the global one (a GPU
        // first seen mid-run) or be pruned on its own, so the global timestamps could mismatch.
        const auto axisConfig = makeTimeAxisConfig(perGpuTimestamps, ctx.maxHistorySeconds, ctx.historyScrollSeconds);

        // Only the clocks the window shows set the scale: not the trim anchor left of it, nor older
        // samples when scrolled back (#1324). The NowBar's smoothed clock counts too, so the bar never
        // exceeds the scale while it eases down from a peak that has left the window.
        const float maxClockMHz = gpuClockReferenceMHz(timeData,
                                                       axisConfig.xMin,
                                                       clockData,
                                                       snap.gpuClockMHz,
                                                       UI::Widgets::currentIfAvailable(smoothed.clockInitialized, smoothed.clockMHz));

        // ========================================
        // Chart 1: Core + Video (all percentages)
        // Utilization, Memory, Clock, Encoder, Decoder
        // ========================================
        ImGui::TextColored(theme.scheme().textPrimary, ICON_FA_VIDEO "  GPU Core & Video (%zu samples)", alignedCount);

        auto gpuCorePlot = [&]()
        {
            const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig("##GPUCoreHistory", axisConfig.xMin, axisConfig.xMax),
                                        plotHeight),
                ctx.chartDataGeneration));
            if (chart.active())
            {
                UI::Widgets::drawCollectingHint(alignedCount); // The same "no data yet" state on every chart (#1013)
                if (!utilData.empty())
                {
                    plotSeries(UTIL_LABEL,
                               timeData.data(),
                               utilData.data(),
                               UI::Format::checkedCount(utilData.size()),
                               theme.scheme().gpuUtilization,
                               theme.scheme().gpuUtilizationFill,
                               seriesStyle(SeriesRole::Primary));
                }

                if (!memData.empty())
                {
                    plotSeries(MEMORY_LABEL,
                               timeData.data(),
                               memData.data(),
                               UI::Format::checkedCount(memData.size()),
                               theme.scheme().gpuMemory,
                               theme.scheme().gpuMemoryFill,
                               seriesStyle(SeriesRole::Secondary, 0));
                }

                // Plot clock as a percentage of gpuClockReferenceMHz(): the window's peak, or the floor
                // when every clock is below it. The label stays fixed; the reference itself is in the
                // tooltip.
                if (caps.hasClockSpeeds && !clockData.empty())
                {
                    normalizeToPercent(clockData, maxClockMHz, clockPercentBuf);
                    const auto clockTimeData = tailAlignedSpan(timeData, clockPercentBuf.size());
                    plotSeries(CLOCK_LABEL,
                               clockTimeData.values.data(),
                               clockPercentBuf.data(),
                               UI::Format::checkedCount(clockTimeData.values.size()),
                               theme.scheme().gpuClock,
                               theme.scheme().gpuClockFill,
                               seriesStyle(SeriesRole::Secondary, 1));
                }

                // Encoder utilization
                if (caps.hasEncoderDecoder && !encoderData.empty())
                {
                    const auto encoderTimeData = tailAlignedSpan(timeData, encoderData.size());
                    plotSeries(ENCODER_LABEL,
                               encoderTimeData.values.data(),
                               encoderData.data(),
                               UI::Format::checkedCount(encoderTimeData.values.size()),
                               theme.scheme().gpuEncoder,
                               std::nullopt,
                               seriesStyle(SeriesRole::Secondary, 2));
                }

                // Decoder utilization
                if (caps.hasEncoderDecoder && !decoderData.empty())
                {
                    const auto decoderTimeData = tailAlignedSpan(timeData, decoderData.size());
                    plotSeries(DECODER_LABEL,
                               decoderTimeData.values.data(),
                               decoderData.data(),
                               UI::Format::checkedCount(decoderTimeData.values.size()),
                               theme.scheme().gpuDecoder,
                               std::nullopt,
                               seriesStyle(SeriesRole::Secondary, 3));
                }

                // Tooltip on hover
                if (ImPlot::IsPlotHovered() && !timeData.empty())
                {
                    const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                    if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                    {
                        // perGpuTimestamps and the GPU history are always the same length and aligned
                        // sample-for-sample, so *idxVal maps directly to the correct history entry.
                        std::vector<UI::Widgets::TooltipRow> rows;
                        if (*idxVal < utilData.size())
                        {
                            rows.push_back({.label = UTIL_LABEL,
                                            .color = theme.scheme().gpuUtilization,
                                            .value = UI::Format::percentCompact(utilData[*idxVal])});
                        }
                        if (*idxVal < memData.size())
                        {
                            const auto pct = static_cast<double>(memData[*idxVal]);
                            const bool haveBytes =
                                *idxVal < memUsedBytesData.size() && *idxVal < memTotalBytesData.size() && memTotalBytesData[*idxVal] > 0;
                            rows.push_back({.label = MEMORY_LABEL,
                                            .color = theme.scheme().gpuMemory,
                                            .value = haveBytes ? UI::Format::bytesUsedTotalPercentCompact(
                                                                     memUsedBytesData[*idxVal], memTotalBytesData[*idxVal], pct)
                                                               : UI::Format::percentCompact(pct)});
                        }
                        // A series that may be shorter than the time axis (it ends at the same newest
                        // sample): its value at the hovered index, if it has one there.
                        const auto valueAt = [&](std::span<const float> series) -> std::optional<double>
                        {
                            const auto aligned = tailAlignedSpan(timeData, series.size());
                            if (*idxVal < aligned.offset)
                            {
                                return std::nullopt;
                            }
                            return static_cast<double>(series[*idxVal - aligned.offset]);
                        };
                        if (caps.hasClockSpeeds && !clockData.empty())
                        {
                            if (const auto clockMHz = valueAt(clockData))
                            {
                                rows.push_back({.label = CLOCK_LABEL,
                                                .color = theme.scheme().gpuClock,
                                                .value = UI::Widgets::formatSampleOrNA(
                                                    *clockMHz,
                                                    [maxClockMHz](double mhz)
                                                    {
                                                        return std::format(
                                                            "{:.0f} MHz ({} of {:.0f} MHz)",
                                                            mhz,
                                                            UI::Format::percentCompact((mhz / static_cast<double>(maxClockMHz)) * 100.0),
                                                            static_cast<double>(maxClockMHz));
                                                    })});
                            }
                        }
                        if (caps.hasEncoderDecoder && !encoderData.empty())
                        {
                            if (const auto encoder = valueAt(encoderData))
                            {
                                rows.push_back({.label = ENCODER_LABEL,
                                                .color = theme.scheme().gpuEncoder,
                                                .value = UI::Format::percentCompact(*encoder)});
                            }
                        }
                        if (caps.hasEncoderDecoder && !decoderData.empty())
                        {
                            if (const auto decoder = valueAt(decoderData))
                            {
                                rows.push_back({.label = DECODER_LABEL,
                                                .color = theme.scheme().gpuDecoder,
                                                .value = UI::Format::percentCompact(*decoder)});
                            }
                        }
                        UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                    }
                }
            }
        };

        // Build now bars for chart 1: utilization, memory, clock, encoder, decoder
        // A bar for a reading this sample couldn't take: N/A in muted text, as the line has a gap,
        // rather than a real-looking 0 (#1111).
        const auto unavailableBar = [&theme](const char* label)
        {
            return NowBar{.valueText = "N/A",
                          .label = label,
                          .tooltipText = UI::InlineText::format("{}: unavailable this sample", label),
                          .value01 = 0.0,
                          .color = theme.scheme().textMuted};
        };
        NowBarList gpuCoreBars;
        gpuCoreBars.push_back(smoothed.utilizationInitialized ? NowBar{.valueText = UI::Format::percentCompact(smoothed.utilizationPercent),
                                                                       .label = UTIL_LABEL,
                                                                       .tooltipText = {},
                                                                       .value01 = UI::Format::percent01(smoothed.utilizationPercent),
                                                                       .color = theme.scheme().gpuUtilization}
                                                              : unavailableBar(UTIL_LABEL));
        if (!smoothed.memoryInitialized)
        {
            gpuCoreBars.push_back(unavailableBar(MEMORY_LABEL));
        }
        else if (snap.memoryTotalBytes > 0)
        {
            // Use the snapshot's own computed percent and raw byte values so the percent
            // and byte figures always come from the same sample and cannot show an
            // impossible combination (e.g. 50% with 8 GiB / 8 GiB).
            gpuCoreBars.push_back(
                {.valueText = UI::Format::percentCompact(smoothed.memoryPercent),
                 .label = MEMORY_LABEL,
                 .tooltipText = UI::InlineText::format("{}: {} ({} / {})",
                                                       MEMORY_LABEL,
                                                       UI::Format::percentCompact(snap.memoryUsedPercent),
                                                       UI::Format::formatBytes(static_cast<double>(snap.memoryUsedBytes)),
                                                       UI::Format::formatBytes(static_cast<double>(snap.memoryTotalBytes))),
                 .value01 = UI::Format::percent01(smoothed.memoryPercent),
                 .color = theme.scheme().gpuMemory});
        }
        else
        {
            gpuCoreBars.push_back(
                {.valueText = UI::Format::percentCompact(smoothed.memoryPercent),
                 .label = MEMORY_LABEL,
                 .tooltipText = UI::Widgets::tooltipRowText(MEMORY_LABEL, UI::Format::percentCompact(smoothed.memoryPercent)),
                 .value01 = UI::Format::percent01(smoothed.memoryPercent),
                 .color = theme.scheme().gpuMemory});
        }
        // Like the fan bar below: present whenever the clock line is, so a zero (unreadable) sample
        // shows N/A instead of removing the bar and shifting every bar after it (#995).
        if (caps.hasClockSpeeds)
        {
            const double clockPercent = (smoothed.clockMHz / static_cast<double>(maxClockMHz)) * 100.0;
            gpuCoreBars.push_back(smoothed.clockInitialized
                                      ? NowBar{.valueText = std::format("{:.0f} MHz", smoothed.clockMHz),
                                               .label = CLOCK_LABEL,
                                               .tooltipText = UI::InlineText::format("{}: {:.0f} MHz ({} of {:.0f} MHz)",
                                                                                     CLOCK_LABEL,
                                                                                     smoothed.clockMHz,
                                                                                     UI::Format::percentCompact(clockPercent),
                                                                                     static_cast<double>(maxClockMHz)),
                                               .value01 = UI::Format::percent01(clockPercent),
                                               .color = theme.scheme().gpuClock}
                                      : NowBar{.valueText = "N/A",
                                               .label = CLOCK_LABEL,
                                               .tooltipText = "Clock: unavailable this sample",
                                               .value01 = 0.0,
                                               .color = theme.scheme().textMuted});
        }
        if (caps.hasEncoderDecoder)
        {
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(smoothed.encoderPercent),
                                   .label = ENCODER_LABEL,
                                   .tooltipText = {},
                                   .value01 = UI::Format::percent01(smoothed.encoderPercent),
                                   .color = theme.scheme().gpuEncoder});
            gpuCoreBars.push_back({.valueText = UI::Format::percentCompact(smoothed.decoderPercent),
                                   .label = DECODER_LABEL,
                                   .tooltipText = {},
                                   .value01 = UI::Format::percent01(smoothed.decoderPercent),
                                   .color = theme.scheme().gpuDecoder});
        }

        // Build thermal bars early so we can calculate max column count for alignment
        NowBarList gpuThermalBars;
        constexpr float maxTempC = 100.0F;
        const float maxPowerW = snap.powerLimitWatts > 0.0 ? static_cast<float>(snap.powerLimitWatts) : 300.0F;
        if (caps.hasTemperature)
        {
            const double tempPercent = (smoothed.temperatureC / static_cast<double>(maxTempC)) * 100.0;
            gpuThermalBars.push_back(smoothed.temperatureInitialized
                                         ? NowBar{.valueText = std::format("{}°C", static_cast<int>(smoothed.temperatureC)),
                                                  .label = TEMP_LABEL,
                                                  .tooltipText = {},
                                                  .value01 = UI::Format::percent01(tempPercent),
                                                  .color = theme.scheme().gpuTemperature}
                                         : unavailableBar(TEMP_LABEL));
        }
        if (caps.hasPowerMetrics)
        {
            const double powerPercent = (smoothed.powerWatts / static_cast<double>(maxPowerW)) * 100.0;
            gpuThermalBars.push_back(smoothed.powerInitialized
                                         ? NowBar{.valueText = std::format("{:.1f}W", smoothed.powerWatts),
                                                  .label = POWER_LABEL,
                                                  .tooltipText = UI::InlineText::format("{}: {:.2Lf} W", POWER_LABEL, smoothed.powerWatts),
                                                  .value01 = UI::Format::percent01(powerPercent),
                                                  .color = theme.scheme().gpuPower}
                                         : unavailableBar(POWER_LABEL));
        }
        // Keep pushing a bar (stable column count) whenever the capability is present, so the
        // now-bar layout doesn't jitter frame-to-frame as fanSpeedAvailable flips on a transient
        // per-poll read failure - but show "N/A" instead of a misleading "0%" for a sample that
        // failed to read the sensor (see fanSpeedAvailable's comment in GPUSnapshot.h).
        if (caps.hasFanSpeed)
        {
            gpuThermalBars.push_back(snap.fanSpeedAvailable ? NowBar{.valueText = std::format("{:.0f}%", smoothed.fanPercent),
                                                                     .label = FAN_LABEL,
                                                                     .tooltipText = {},
                                                                     .value01 = UI::Format::percent01(smoothed.fanPercent),
                                                                     .color = theme.scheme().gpuFan}
                                                            : NowBar{.valueText = "N/A",
                                                                     .label = FAN_LABEL,
                                                                     .tooltipText = "Fan: unavailable this sample",
                                                                     .value01 = 0.0,
                                                                     .color = theme.scheme().textMuted});
        }

        // Use max bar count across both charts for x-axis alignment
        const size_t gpuNowBarColumns = std::max(gpuCoreBars.size(), gpuThermalBars.size());

        renderHistoryWithNowBars(cache.coreLayoutIds[entryIndex].c_str(), plotHeight, gpuCorePlot, gpuCoreBars, false, gpuNowBarColumns);
        countPlot();

        // Show notes for unavailable core metrics
        {
            UnavailableMetrics unavailableCoreNotes;
            if (!caps.hasClockSpeeds)
            {
                unavailableCoreNotes.add("clock speed");
            }
            if (!caps.hasEncoderDecoder)
            {
                unavailableCoreNotes.add("encoder/decoder utilization");
            }
            renderUnavailableMetricsNote(unavailableCoreNotes.names(), theme.scheme().textMuted);
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
                const UI::Widgets::HistoryChart chart(UI::Widgets::withDataGeneration(
                    UI::Widgets::withHeight(UI::Widgets::percentHistoryConfig("##GPUThermalHistory", axisConfig.xMin, axisConfig.xMax),
                                            plotHeight),
                    ctx.chartDataGeneration));
                if (chart.active())
                {
                    UI::Widgets::drawCollectingHint(alignedCount); // The same "no data yet" state on every chart (#1013)
                    // Temperature (normalized to 0-100%)
                    if (caps.hasTemperature && !tempData.empty())
                    {
                        normalizeToPercent(tempData, maxTempC, tempPercentBuf);
                        const auto tempTimeData = tailAlignedSpan(timeData, tempPercentBuf.size());
                        plotSeries(TEMP_LABEL,
                                   tempTimeData.values.data(),
                                   tempPercentBuf.data(),
                                   UI::Format::checkedCount(tempTimeData.values.size()),
                                   theme.scheme().gpuTemperature,
                                   std::nullopt,
                                   seriesStyle(SeriesRole::Primary));
                    }

                    // Power (normalized to actual reference watts; includes fallback note when limit is unavailable)
                    if (caps.hasPowerMetrics && !powerData.empty())
                    {
                        normalizeToPercent(powerData, maxPowerW, powerPercentBuf);
                        const auto powerTimeData = tailAlignedSpan(timeData, powerPercentBuf.size());
                        plotSeries(POWER_LABEL,
                                   powerTimeData.values.data(),
                                   powerPercentBuf.data(),
                                   UI::Format::checkedCount(powerTimeData.values.size()),
                                   theme.scheme().gpuPower,
                                   std::nullopt,
                                   seriesStyle(SeriesRole::Secondary, 0));
                    }

                    // Fan speed (already a percentage)
                    if (caps.hasFanSpeed && !fanData.empty())
                    {
                        const auto fanTimeData = tailAlignedSpan(timeData, fanData.size());
                        plotSeries(FAN_LABEL,
                                   fanTimeData.values.data(),
                                   fanData.data(),
                                   UI::Format::checkedCount(fanTimeData.values.size()),
                                   theme.scheme().gpuFan,
                                   std::nullopt,
                                   seriesStyle(SeriesRole::Secondary, 1));
                    }

                    // Tooltip on hover
                    if (ImPlot::IsPlotHovered() && !timeData.empty())
                    {
                        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
                        if (const auto idxVal = hoveredIndexFromPlotX(timeData, mouse.x))
                        {
                            const auto valueAt = [&](std::span<const float> series) -> std::optional<double>
                            {
                                const auto aligned = tailAlignedSpan(timeData, series.size());
                                if (*idxVal < aligned.offset)
                                {
                                    return std::nullopt;
                                }
                                return static_cast<double>(series[*idxVal - aligned.offset]);
                            };
                            // "N/A" for a sample with no reading, such as while the GPU was missing (#1146).
                            const auto ofReference = [](double value, double reference, std::string_view unit, std::string_view note)
                            {
                                return UI::Widgets::formatSampleOrNA(value,
                                                                     [&](double reading)
                                                                     {
                                                                         return std::format(
                                                                             "{:.0f}{} ({} of {:.0f}{}{})",
                                                                             reading,
                                                                             unit,
                                                                             UI::Format::percentCompact((reading / reference) * 100.0),
                                                                             reference,
                                                                             unit,
                                                                             note);
                                                                     });
                            };
                            std::vector<UI::Widgets::TooltipRow> rows;
                            if (caps.hasTemperature && !tempData.empty())
                            {
                                if (const auto temp = valueAt(tempData))
                                {
                                    rows.push_back({.label = TEMP_LABEL,
                                                    .color = theme.scheme().gpuTemperature,
                                                    .value = ofReference(*temp, static_cast<double>(maxTempC), "°C", "")});
                                }
                            }
                            if (caps.hasPowerMetrics && !powerData.empty())
                            {
                                if (const auto power = valueAt(powerData))
                                {
                                    // Without a reported power limit the line is scaled to an assumed one.
                                    rows.push_back({.label = POWER_LABEL,
                                                    .color = theme.scheme().gpuPower,
                                                    .value = ofReference(*power,
                                                                         static_cast<double>(maxPowerW),
                                                                         " W",
                                                                         snap.powerLimitWatts > 0.0 ? "" : ", assumed limit")});
                                }
                            }
                            if (caps.hasFanSpeed && !fanData.empty())
                            {
                                if (const auto fan = valueAt(fanData))
                                {
                                    // NaN marks a sample where the fan couldn't be read (see GPUModel::publish()).
                                    // The float is formatted directly: fanSpeedPercent is left unclamped (see
                                    // GPUModel::computeSnapshot), and casting an out-of-range float to an integer
                                    // is undefined behaviour.
                                    rows.push_back(
                                        {.label = FAN_LABEL,
                                         .color = std::isnan(*fan) ? theme.scheme().textMuted : theme.scheme().gpuFan,
                                         .value = UI::Widgets::formatSampleOrNA(*fan, [](double v) { return std::format("{:.0f}%", v); })});
                                }
                            }
                            UI::Widgets::renderHistoryTooltip(timeData[*idxVal], rows);
                        }
                    }
                }
            };

            // Thermal bars were already built above for alignment calculation, one for each capability
            // that brought this chart here, so there is always at least one. Rendered with the same
            // column count as the core chart for x-axis alignment.
            renderHistoryWithNowBars(
                cache.thermalLayoutIds[entryIndex].c_str(), plotHeight, gpuThermalPlot, gpuThermalBars, false, gpuNowBarColumns);
            countPlot();

            // Show notes for unavailable metrics
            UnavailableMetrics unavailableNotes;
            if (!caps.hasTemperature)
            {
                unavailableNotes.add("temperature");
            }
            if (!caps.hasPowerMetrics)
            {
                unavailableNotes.add("power draw");
            }
            if (!caps.hasFanSpeed)
            {
                unavailableNotes.add("fan speed");
            }
            renderUnavailableMetricsNote(unavailableNotes.names(), theme.scheme().textMuted);
        }

        ImGui::Unindent();
        ImGui::Spacing();
    }
}

} // namespace App::GpuSection
