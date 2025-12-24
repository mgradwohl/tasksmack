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

/// Clamp value to int32_t range.
/// If value exceeds int32_t max/min, clamps to int32_t::max or int32_t::min.
/// Use this for safe narrowing conversions where clamping is preferred over fallback.
template<std::integral From> [[nodiscard]] constexpr auto clampToI32(From value) noexcept -> int32_t
{
    if (value < std::numeric_limits<int32_t>::min())
    {
        return std::numeric_limits<int32_t>::min();
    }
    if (value > std::numeric_limits<int32_t>::max())
    {
        return std::numeric_limits<int32_t>::max();
    }
    // Explicit conversion is safe here because we've verified the value is in range for int32_t
    return static_cast<int32_t>(value);
}

} // namespace Domain::Numeric
