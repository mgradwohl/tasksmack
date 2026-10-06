#pragma once

// The process details GPU tab's "is there anything to show" decision, extracted from
// ProcessDetailsPanel::renderContent() so it is unit-testable without a live ImGui context.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>

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
    NoUsage,     ///< It can, and this process has used no GPU since it was selected
    Usage,       ///< Its usage and charts
};

/// The GPU tab's content. Where the GPU probe has no per-process metrics (DRM- or ROCm-only Linux,
/// no usable GPU probe), every process reads no GPU use, so "has not used a GPU" would claim what
/// cannot be seen: that case says per-process GPU usage is not available instead.
///
/// @param perProcessGpuSupported  ProcessColumnAvailability::perProcessGpuSupported().
/// @param hasUsageToShow          hasGpuUsageToShow().
[[nodiscard]] constexpr GpuTabContent gpuTabContent(bool perProcessGpuSupported, bool hasUsageToShow) noexcept
{
    if (!perProcessGpuSupported)
    {
        return GpuTabContent::Unavailable;
    }
    return hasUsageToShow ? GpuTabContent::Usage : GpuTabContent::NoUsage;
}

/// Whether the GPU tab shows its "Per-GPU Breakdown" under the usage table. With a single GPU its
/// utilization, memory and engines are the "GPU Usage" table's, so it only repeated them (#1207);
/// it earns its place once the process uses more than one GPU.
[[nodiscard]] constexpr bool shouldShowPerGpuBreakdown(std::size_t gpuCount) noexcept
{
    return gpuCount > 1;
}

} // namespace App::Detail
