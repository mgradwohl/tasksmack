#pragma once

#include <algorithm>
#include <concepts>
#include <limits>
#include <utility>

namespace Domain::Numeric
{

template<typename T>
    requires(std::integral<T> || std::floating_point<T>)
[[nodiscard]] constexpr auto toDouble(T value) noexcept -> double
{
    return static_cast<double>(value);
}

[[nodiscard]] inline auto clampPercentToFloat(double percent) noexcept -> float
{
    const double clamped = std::clamp(percent, 0.0, 100.0);
    return static_cast<float>(clamped);
}

/// Safe narrowing conversion with fallback value.
/// Returns fallback if value is out of range for target type.
/// Use this instead of assert-based conversions to ensure safety in release builds.
template<std::integral To, std::integral From> [[nodiscard]] constexpr auto narrowOr(From value, To fallback) noexcept -> To
{
    if (!std::in_range<To>(value))
    {
        return fallback;
    }
    // Explicit conversion is safe here because we've verified the value is in range for the target type
    return static_cast<To>(value);
}

} // namespace Domain::Numeric
