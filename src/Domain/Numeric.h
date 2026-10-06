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

/// counterDelta() for a cumulative counter the OS keeps in only @p bits bits and lets wrap to 0
/// past its maximum -- Windows' per-process page-fault count is a 32-bit ULONG (#1184). A reading
/// below the previous one is one wrap, so the delta is taken modulo 2^bits instead of reading 0 for
/// that interval. @p bits of 0 or at least T's width means the counter does not wrap (counterDelta()).
/// A reading (either one) too big for @p bits cannot have come from such a counter: the delta is 0,
/// whether or not the readings increased.
template<std::unsigned_integral T> [[nodiscard]] constexpr auto wrappingCounterDelta(T current, T previous, unsigned bits) noexcept -> T
{
    if (bits == 0 || bits >= static_cast<unsigned>(std::numeric_limits<T>::digits))
    {
        return counterDelta(current, previous);
    }
    const T maxValue = (T{1} << bits) - T{1};
    if (previous > maxValue || current > maxValue)
    {
        return T{};
    }
    if (current >= previous)
    {
        return current - previous;
    }
    return (maxValue - previous) + current + T{1};
}

/// counterRate() for a counter that wraps at 2^bits (wrappingCounterDelta()).
template<std::unsigned_integral T>
[[nodiscard]] constexpr auto wrappingCounterRate(T current, T previous, double elapsedSeconds, unsigned bits) noexcept -> double
{
    if (elapsedSeconds <= 0.0)
    {
        return 0.0;
    }
    return toDouble(wrappingCounterDelta(current, previous, bits)) / elapsedSeconds;
}

} // namespace Domain::Numeric
