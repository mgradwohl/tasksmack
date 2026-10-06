#pragma once

// Tail alignment of chart series: every history ends at the newest sample, so a series shorter than
// its chart's time axis covers the axis's *last* entries. The one implementation of that rule for
// drawing a series and for reading it back at a hovered index (#1180); per-site hand-written
// alignment was the source of several of the October 2026 review's alignment findings.
//
// Free of ImGui so it is unit-testable directly.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace UI::Widgets
{

/// Where a series of @p seriesCount samples starts on a time axis of @p axisCount entries. A series
/// longer than the axis starts at 0.
[[nodiscard]] constexpr std::size_t tailAlignedOffset(std::size_t axisCount, std::size_t seriesCount) noexcept
{
    return axisCount - std::min(seriesCount, axisCount);
}

template<typename T> struct TailAlignedSpan
{
    std::span<const T> values;
    std::size_t offset = 0;
};

/// The newest @p count entries of @p data, and where they start in it.
template<typename T> [[nodiscard]] inline TailAlignedSpan<T> tailAlignedSpan(std::span<const T> data, std::size_t count)
{
    const std::size_t offset = tailAlignedOffset(data.size(), count);
    return {data.subspan(offset), offset};
}

template<typename T> [[nodiscard]] inline TailAlignedSpan<T> tailAlignedSpan(const std::vector<T>& data, std::size_t count)
{
    return tailAlignedSpan(std::span<const T>(data), count);
}

/// The sample of a series drawn from axis index @p offset (tailAlignedOffset()) at axis index
/// @p axisIndex, or nullopt where the series has no sample. A stored NaN (a gap) is returned as it is.
template<typename T>
[[nodiscard]] inline std::optional<double> tailAlignedSampleAt(std::span<const T> series, std::size_t offset, std::size_t axisIndex)
{
    if (axisIndex < offset || axisIndex - offset >= series.size())
    {
        return std::nullopt;
    }
    return static_cast<double>(series[axisIndex - offset]);
}

} // namespace UI::Widgets
