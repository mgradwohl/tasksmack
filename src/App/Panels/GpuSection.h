#pragma once

#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "UI/FillPlotLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>

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

/// Context struct containing all state needed to render the GPU section.
/// This allows the render function to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Model (non-owning pointer)
    const Domain::GPUPublication* publication = nullptr;

    // History configuration
    double maxHistorySeconds = 300.0;
    double historyScrollSeconds = 0.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values per GPU (map keyed by GPU ID)
    std::unordered_map<std::string, SmoothedGPU>* smoothedGPUs = nullptr;

    // The tab's state for the shared chart-height rule (#959). The fill needs the previous frame's
    // measurement, so with null the charts stay at the minimum height every frame.
    UI::Widgets::PlotFillState* plotFill = nullptr;
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
