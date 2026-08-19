#pragma once

#include <algorithm>
#include <concepts>
#include <utility>

namespace Domain::Numeric
{

template<typename T>
    requires(std::integral<T> || std::floating_point<T>)
[[nodiscard]] constexpr auto toDouble(T value) noexcept -> double
{
    return static_cast<double>(value);
}

template<std::unsigned_integral T> [[nodiscard]] constexpr auto counterDelta(T current, T previous) noexcept -> T
{
    return current >= previous ? current - previous : T{};
}

template<std::unsigned_integral T> [[nodiscard]] constexpr auto counterRate(T current, T previous, double elapsedSeconds) noexcept -> double
{
    if (elapsedSeconds <= 0.0)
    {
        return 0.0;
    }
    return toDouble(counterDelta(current, previous)) / elapsedSeconds;
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
