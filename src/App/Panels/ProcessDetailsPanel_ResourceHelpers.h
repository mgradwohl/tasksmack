#pragma once

// Helpers for the process details Resources chart's optional series (GDI objects on Windows),
// extracted from ProcessDetailsPanel so they are unit-testable without a live ImGui context.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace App::Detail
{

/// Where a series of @p seriesCount samples starts within a chart of @p alignedCount timestamps.
/// Every history ends at the newest sample, so a shorter series covers the *last* timestamps: it
/// must be drawn from this offset, not from the first timestamp (#1001).
[[nodiscard]] constexpr std::size_t seriesTimeOffset(std::size_t alignedCount, std::size_t seriesCount) noexcept
{
    return alignedCount - std::min(seriesCount, alignedCount);
}

/// The sample of a tail-aligned series at chart index @p chartIndex, or nullopt when the series
/// has no sample there or the sample is missing (stored as NaN) -- printing a NaN through
/// llround() showed -9,223,372,036,854,775,808 in the tooltip (#1000).
[[nodiscard]] inline std::optional<double> seriesValueAt(std::span<const double> series, std::size_t offset, std::size_t chartIndex)
{
    if (chartIndex < offset || chartIndex - offset >= series.size())
    {
        return std::nullopt;
    }
    const double value = series[chartIndex - offset];
    if (std::isnan(value))
    {
        return std::nullopt;
    }
    return value;
}

/// Whether any sample in @p series is present (not NaN). A series with none is not drawn and has
/// no legend entry (#1000).
[[nodiscard]] inline bool hasAnySample(std::span<const double> series)
{
    return std::ranges::any_of(series, [](double value) { return !std::isnan(value); });
}

/// A smoothed NowBar value whose reading can be missing (GDI objects on Windows).
struct SmoothedOptionalReading
{
    double value = 0.0;
    bool available = false; // whether the latest sample had a reading
};

/// One smoothing step for a reading that can be missing. A missing reading leaves the value where it
/// was and marks it unavailable, so the NowBar shows N/A as the line shows a gap, rather than easing
/// toward 0; the next reading then starts afresh instead of smoothing from the stale value (#1148).
/// @p canSmooth is false on the first frame or when no time has passed.
[[nodiscard]] constexpr SmoothedOptionalReading
smoothOptionalReading(SmoothedOptionalReading state, std::optional<double> reading, double alpha, bool canSmooth) noexcept
{
    if (!reading.has_value())
    {
        state.available = false;
        return state;
    }
    const double target = std::max(0.0, *reading);
    state.value = (canSmooth && state.available) ? std::max(0.0, state.value + (alpha * (target - state.value))) : target;
    state.available = true;
    return state;
}

} // namespace App::Detail
