#pragma once

#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"
#include "UI/FillPlotLayout.h"
#include "UI/Format.h"
#include "UI/RateAxis.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace App::GpuSection
{

/// Smoothed GPU values for a single GPU device.
/// Stored per GPU ID to handle multiple GPUs.
struct SmoothedGPU
{
    double utilizationPercent = 0.0;
    double memoryPercent = 0.0;
    double temperatureC = 0.0;
    double powerWatts = 0.0;
    double clockMHz = 0.0;
    double encoderPercent = 0.0;
    double decoderPercent = 0.0;
    double fanPercent = 0.0;
    // Whether each value holds a reading. Utilization, temperature, power, clock and fan are left
    // alone on a sample that could not read them (their bars show N/A) and restart from the next
    // reading, rather than easing toward 0 (#1111). Clock and fan were drawn raw before, so they
    // stepped while the bars beside them glided (#1012).
    bool utilizationInitialized = false;
    bool memoryInitialized = false;
    bool temperatureInitialized = false;
    bool powerInitialized = false;
    bool clockInitialized = false;
    bool fanInitialized = false;
    bool initialized = false;
};

/// Why the GPU tab has nothing to chart, if it does not.
enum class EmptyReason : std::uint8_t
{
    None, ///< There is data; render the tab.
    /// GPU data could not be read: either nothing has been published at all (the probe is missing
    /// or its read failed), or something was published but enumerating the devices failed, so its
    /// empty device list means "could not look" rather than "found none".
    Unavailable,
    NoDevices,  ///< Enumeration succeeded and found no GPU.
    NoReadings, ///< GPUs are known, but the latest read returned no counters for any of them.
};

/// Classifies the GPU tab's empty state from what the model published (#927).
///
/// The reasons need different words, and each of the obvious shortcuts says something false:
///
///   - A null publication is not a wait. GPUModel publishes on every successful refresh, including
///     one that finds zero GPUs, and the panel takes its first refresh before it ever renders -- so
///     it means the probe is missing or its read failed.
///   - An empty snapshot list does not mean there is no GPU. The device list is published
///     separately, so known devices with no counters is a failed or momentarily empty read.
///   - An empty device list does not mean there is no GPU either, unless enumeration succeeded.
///     GPUModel catches a failed enumeration and carries on with an empty list, and later reads
///     still publish; "no GPU detected" would then be a guess presented as a finding.
///
/// @param hasPublication  The model has published at least once.
/// @param devicesKnown    Device enumeration succeeded (GPUPublication::gpuInfoKnown).
/// @param deviceCount     Devices enumerated.
/// @param snapshotCount   Devices the latest read returned counters for.
[[nodiscard]] constexpr EmptyReason
classifyEmptyState(bool hasPublication, bool devicesKnown, std::size_t deviceCount, std::size_t snapshotCount) noexcept
{
    if (!hasPublication)
    {
        return EmptyReason::Unavailable;
    }
    if (snapshotCount > 0)
    {
        return EmptyReason::None;
    }
    if (deviceCount > 0)
    {
        return EmptyReason::NoReadings;
    }
    return devicesKnown ? EmptyReason::NoDevices : EmptyReason::Unavailable;
}

/// The capabilities that apply to one GPU. GPUCapabilities describes the probe as a whole -- on a
/// hybrid Windows laptop NVML's sensor capabilities are set because the NVIDIA GPU has them -- so
/// each sensor series is kept only if this adapter reports it (GPUInfo::sensorCapabilities), rather
/// than drawn as a line stuck at 0 (#1040). Without per-adapter sensors the probe's apply as is.
/// Utilization and per-process metrics are not per adapter and are left alone.
[[nodiscard]] constexpr Platform::GPUCapabilities
capabilitiesForGpu(Platform::GPUCapabilities caps, const std::optional<Platform::GPUCapabilities>& adapterSensors) noexcept
{
    if (adapterSensors.has_value())
    {
        caps.hasTemperature = caps.hasTemperature && adapterSensors->hasTemperature;
        caps.hasHotspotTemp = caps.hasHotspotTemp && adapterSensors->hasHotspotTemp;
        caps.hasPowerMetrics = caps.hasPowerMetrics && adapterSensors->hasPowerMetrics;
        caps.hasClockSpeeds = caps.hasClockSpeeds && adapterSensors->hasClockSpeeds;
        caps.hasFanSpeed = caps.hasFanSpeed && adapterSensors->hasFanSpeed;
        caps.hasPCIeMetrics = caps.hasPCIeMetrics && adapterSensors->hasPCIeMetrics;
        caps.hasEncoderDecoder = caps.hasEncoderDecoder && adapterSensors->hasEncoderDecoder;
    }
    return caps;
}

/// One GPU the tab draws: its enumeration entry and its latest snapshot, either of which may be
/// missing. gpuId keys the GPU's ImGui IDs and points into the publication it was built from.
struct GpuDrawEntry
{
    std::string_view gpuId;
    const Platform::GPUInfo* info = nullptr;       ///< Null: the read returned a GPU enumeration did not list.
    const Domain::GPUSnapshot* snapshot = nullptr; ///< Null: no reading for this GPU this sample.
};

/// The GPUs the tab draws, in order: every enumerated GPU in enumeration order, whether or not the
/// latest read returned it, then any snapshot for a GPU enumeration did not list (all of them, if
/// enumeration failed), in the published order. A GPU missing from one read keeps its slot and its
/// UI state instead of vanishing and shifting the GPUs after it, whose state used to be keyed by
/// position (#1163).
///
/// Written into @p entries, replacing what it held but keeping its capacity, so the tab can rebuild the
/// list every frame without allocating (#1171).
inline void gpuDrawList(const Domain::GPUPublication& publication, std::vector<GpuDrawEntry>& entries)
{
    entries.clear();
    entries.reserve(publication.gpuInfo.size() + publication.snapshots.size());
    const auto listed = [&entries](std::string_view gpuId)
    {
        return std::ranges::any_of(entries, [gpuId](const GpuDrawEntry& entry) { return entry.gpuId == gpuId; });
    };

    for (const auto& info : publication.gpuInfo)
    {
        if (listed(info.id))
        {
            continue;
        }
        const auto snapshotIt = std::ranges::find(publication.snapshots, info.id, &Domain::GPUSnapshot::gpuId);
        entries.push_back({
            .gpuId = info.id,
            .info = &info,
            .snapshot = (snapshotIt != publication.snapshots.end()) ? &*snapshotIt : nullptr,
        });
    }
    for (const auto& snapshot : publication.snapshots)
    {
        if (!listed(snapshot.gpuId))
        {
            entries.push_back({.gpuId = snapshot.gpuId, .info = nullptr, .snapshot = &snapshot});
        }
    }
}

/// gpuDrawList() into a new vector.
[[nodiscard]] inline std::vector<GpuDrawEntry> gpuDrawList(const Domain::GPUPublication& publication)
{
    std::vector<GpuDrawEntry> entries;
    gpuDrawList(publication, entries);
    return entries;
}

/// The Overview header's "VRAM" figure: memory totals summed over discrete GPUs only. An integrated
/// GPU's total is the share of system RAM it may borrow (on Windows DXGI's SharedSystemMemory, about
/// half of RAM), so adding it double-counted RAM as VRAM on almost every laptop (#1114).
[[nodiscard]] inline std::uint64_t totalDedicatedVramBytes(std::span<const Domain::GPUSnapshot> snapshots) noexcept
{
    std::uint64_t total = 0;
    for (const auto& snapshot : snapshots)
    {
        if (!snapshot.isIntegrated)
        {
            total += snapshot.memoryTotalBytes;
        }
    }
    return total;
}

/// The GPU's collapsing-header label: name, a discrete GPU's VRAM size, its kind, and "Sleeping"
/// while the probe is leaving a runtime-suspended GPU alone (#1117). The "###" suffix keeps the
/// header's ImGui id stable while the label changes, so it doesn't re-expand as the GPU wakes.
[[nodiscard]] inline std::string
gpuHeaderLabel(std::string_view icon, std::string_view name, bool isIntegrated, std::uint64_t memoryTotalBytes, bool suspended)
{
    std::string label = std::string(icon) + " " + std::string(name);
    if (!isIntegrated && memoryTotalBytes > 0)
    {
        label += ", " + UI::Format::formatBytes(static_cast<double>(memoryTotalBytes)) + " VRAM";
    }
    label += isIntegrated ? " [Shared Memory]" : " [Discrete]";
    if (suspended)
    {
        label += " (Sleeping)";
    }
    label += "###gpuHeader";
    return label;
}

/// Lowest reference the clock line and bar are scaled against, so an idle GPU's few hundred MHz
/// don't fill the chart.
inline constexpr float GPU_CLOCK_REFERENCE_FLOOR_MHZ = 2000.0F;

/// The clock the GPU clock line and bar are drawn as a percentage of: the highest clock in the
/// history (no probe reports the device's maximum), floored at GPU_CLOCK_REFERENCE_FLOOR_MHZ. The
/// scale moves only when a new peak arrives or the old one ages out, not with every sample, and no
/// sample in the history exceeds 100 % (#994).
[[nodiscard]] inline float gpuClockReferenceMHz(std::span<const float> clockHistory, std::uint32_t currentClockMHz) noexcept
{
    float reference = std::max(GPU_CLOCK_REFERENCE_FLOOR_MHZ, static_cast<float>(currentClockMHz));
    for (const float clock : clockHistory)
    {
        if (std::isfinite(clock))
        {
            reference = std::max(reference, clock);
        }
    }
    return reference;
}

/// gpuClockReferenceMHz() over only the clock samples the chart's window shows: those at x >= @p xMin
/// on @p timeAxis, to whose tail @p clockHistory is aligned (UI::Widgets::maxOfSeriesSince()).
///
/// The history holds the trim anchor just left of the window (#1016) and, when the chart is scrolled
/// back, older samples too. Neither is drawn, so neither may set the 100 % mark: a boost spike that had
/// just scrolled out kept the idle clock line drawn low against it (#1324), as #1145 fixed for the rate
/// axes. The current clock still counts, since the bar shows it.
[[nodiscard]] inline float gpuClockReferenceMHz(std::span<const double> timeAxis,
                                                double xMin,
                                                std::span<const float> clockHistory,
                                                std::uint32_t currentClockMHz) noexcept
{
    // The peak is one of the float samples (or 0 when none is visible), so narrowing it back is exact.
    const auto visiblePeakMHz = static_cast<float>(UI::Widgets::maxOfSeriesSince(timeAxis, xMin, clockHistory));
    return std::max(gpuClockReferenceMHz({}, currentClockMHz), visiblePeakMHz);
}

/// What the GPU tab keeps from frame to frame, so that once warmed up drawing it allocates nothing
/// (#1171): its scratch buffers and draw list, reused, and the strings built from a publication --
/// header labels and chart layout IDs, one per draw-list entry -- rebuilt only when a new publication
/// arrives. Owned by the panel; one per GPU tab. UI thread only.
struct FrameCache
{
    std::vector<GpuDrawEntry> drawList;
    std::vector<float> clockPercent;
    std::vector<float> temperaturePercent;
    std::vector<float> powerPercent;

    /// Per draw-list entry, built from the publication named below.
    std::vector<std::string> headerLabels;
    std::vector<std::string> coreLayoutIds;
    std::vector<std::string> thermalLayoutIds;
    const Domain::GPUPublication* labelsPublication = nullptr;
    std::uint64_t labelsVersion = 0;
};

/// Context struct containing all state needed to render the GPU section.
/// This allows the render function to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Model (non-owning pointer)
    const Domain::GPUPublication* publication = nullptr;
    // Generation of the histories charted (UI::Widgets::nextChartDataGeneration()), so the charts keep
    // their reduced points until it changes (HistoryChartConfig::dataGeneration, #1139). 0: none.
    std::uint64_t chartDataGeneration = 0;

    // History configuration
    double maxHistorySeconds = 300.0;
    double historyScrollSeconds = 0.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values per GPU (map keyed by GPU ID)
    std::unordered_map<std::string, SmoothedGPU>* smoothedGPUs = nullptr;

    // Shares the tab's height among every expanded GPU's charts, as the other tabs' charts do
    // (#959). Null keeps the fixed default height.
    UI::Widgets::FillPlotLayout* fill = nullptr;

    // Kept across frames by the caller so drawing allocates nothing (#1171). Null: a fresh one for the
    // frame, which draws the same but rebuilds everything.
    FrameCache* cache = nullptr;
};

/// Render the GPU section with utilization, memory, thermal, and power charts.
/// @param ctx Render context containing model and smoothed values
void renderGpuSection(RenderContext& ctx);

/// Update smoothed values for a single GPU.
/// @param gpuId The GPU identifier
/// @param snap Current GPU snapshot
/// @param ctx Render context (uses refreshInterval and lastDeltaSeconds)
void updateSmoothedGPU(const std::string& gpuId, const Domain::GPUSnapshot& snap, RenderContext& ctx);

} // namespace App::GpuSection
