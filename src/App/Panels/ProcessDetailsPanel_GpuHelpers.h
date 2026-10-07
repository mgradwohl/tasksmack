#pragma once

// The process details GPU tab's "is there anything to show" decision, extracted from
// ProcessDetailsPanel::renderContent() so it is unit-testable without a live ImGui context.

#include "UI/Format.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>

namespace App::Detail
{

/// Whether the process details GPU tab shows its usage and charts, rather than "No GPU usage
/// detected for this process".
///
/// True while the process uses a GPU now *or* its retained history holds any GPU use: the charts
/// used to disappear the moment the process went idle on the GPU, taking the history with them
/// (#1014). Once the idle samples have pushed every non-zero sample out of the window, there is
/// nothing left to chart and the message returns.
template<std::ranges::input_range UtilHistory, std::ranges::input_range MemoryHistory>
[[nodiscard]] bool hasGpuUsageToShow(std::uint64_t currentMemoryBytes,
                                     double currentUtilPercent,
                                     bool hasGpuDevices,
                                     const UtilHistory& utilHistory,
                                     const MemoryHistory& memoryHistory)
{
    if (currentMemoryBytes > 0 || currentUtilPercent > 0.0 || hasGpuDevices)
    {
        return true;
    }
    const auto isUsed = [](double value)
    {
        return value > 0.0;
    };
    return std::ranges::any_of(utilHistory, isUsed) || std::ranges::any_of(memoryHistory, isUsed);
}

/// What the process details GPU tab shows (#1210).
enum class GpuTabContent : std::uint8_t
{
    Unavailable, ///< Per-process GPU usage cannot be observed on this system: say so
    NoReadings,  ///< It can, but no read of it has succeeded in the retained history: say so
    NoUsage,     ///< It can, and the readings retained show no GPU use
    Usage,       ///< Its usage and charts
};

/// The GPU tab's content. Where the GPU probe has no per-process metrics (DRM- or ROCm-only Linux,
/// no usable GPU probe), every process reads no GPU use, so "has not used a GPU" would claim what
/// cannot be seen: that case says per-process GPU usage is not available instead. Likewise where it
/// has them but every retained read failed (the history is only gaps): that is no readings yet, not
/// a measured lack of use (#1210).
///
/// @param perProcessGpuSupported  ProcessColumnAvailability::perProcessGpuSupported().
/// @param hasUsageToShow          hasGpuUsageToShow().
/// @param hasAnyReading           Whether the retained GPU histories hold any reading (not only gaps).
[[nodiscard]] constexpr GpuTabContent gpuTabContent(bool perProcessGpuSupported, bool hasUsageToShow, bool hasAnyReading = true) noexcept
{
    if (!perProcessGpuSupported)
    {
        return GpuTabContent::Unavailable;
    }
    if (hasUsageToShow)
    {
        return GpuTabContent::Usage;
    }
    return hasAnyReading ? GpuTabContent::NoUsage : GpuTabContent::NoReadings;
}

/// The GPU tab's "No GPU usage" explanation, for a history window of `historySeconds` (#1210).
/// hasGpuUsageToShow() looks only at the retained history, so a process that used the GPU before
/// the window gets this message too: it says "in the last 5m", in formatDuration()'s grammar, rather
/// than claiming the process never used one. Built when the window changes, not every frame.
[[nodiscard]] inline std::string noGpuUsageDetail(double historySeconds)
{
    // "Retained history (up to ...)", not "the last ...": histories are cleared when a process is
    // selected, so right after selecting it far less than the configured length has been observed.
    return std::format("This process has not used a GPU in its retained history (up to {}).",
                       UI::Format::formatDuration(historySeconds, UI::Format::DurationStyle::Compact));
}

/// A process's GPU utilization as the GPU tab shows it: "N/A" where the GPU probe reports per-process
/// memory but not utilization (NVML on Linux, #1210), since every process would read a measured 0%.
[[nodiscard]] inline std::string gpuUtilizationText(bool perProcessUtilizationSupported, double percent)
{
    return perProcessUtilizationSupported ? UI::Format::percentOneDecimal(percent) : std::string("N/A");
}

/// Whether the GPU tab shows its "Per-GPU Breakdown" under the usage table. With a single GPU its
/// utilization, memory and engines are the "GPU Usage" table's, so it only repeated them (#1207);
/// it earns its place once the process uses more than one GPU.
[[nodiscard]] constexpr bool shouldShowPerGpuBreakdown(std::size_t gpuCount) noexcept
{
    return gpuCount > 1;
}

} // namespace App::Detail
