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

/// Whether the GPU tab shows its "Per-GPU Breakdown" under the usage table. With a single GPU its
/// utilization, memory and engines are the "GPU Usage" table's, so it only repeated them (#1207);
/// it earns its place once the process uses more than one GPU.
[[nodiscard]] constexpr bool shouldShowPerGpuBreakdown(std::size_t gpuCount) noexcept
{
    return gpuCount > 1;
}

} // namespace App::Detail
