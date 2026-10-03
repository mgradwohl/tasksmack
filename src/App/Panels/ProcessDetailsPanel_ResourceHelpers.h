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

} // namespace App::Detail
