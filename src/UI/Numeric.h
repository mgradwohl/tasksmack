#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <utility>

namespace UI::Numeric
{

[[nodiscard]] constexpr auto clampPercent(double percent) noexcept -> double
{
    return std::clamp(percent, 0.0, 100.0);
}

[[nodiscard]] constexpr auto percent01(double percent) noexcept -> double
{
    return clampPercent(percent) / 100.0;
}

template<std::integral To, std::integral From> [[nodiscard]] constexpr auto narrowOr(From value, To fallback) noexcept -> To
{
    if (!std::in_range<To>(value))
    {
        return fallback;
    }

    // Explicit conversion to satisfy strict warnings-as-errors (e.g. -Wconversion / -Wsign-conversion).
    return static_cast<To>(value);
}

// ImPlot series counts are int; keep conversion explicit + checked.
[[nodiscard]] constexpr auto checkedCount(std::size_t value) noexcept -> int
{
    return narrowOr<int>(value, std::numeric_limits<int>::max());
}

template<typename T>
    requires(std::integral<T> || std::floating_point<T>)
[[nodiscard]] constexpr auto toDouble(T value) noexcept -> double
{
    // Explicit conversion to satisfy strict warnings-as-errors (e.g. -Wimplicit-int-float-conversion, -Wdouble-promotion).
    return static_cast<double>(value);
}

[[nodiscard]] constexpr auto toFloatNarrow(double value) noexcept -> float
{
    // Narrowing: some ImGui/ImPlot APIs store or accept values as float.
    return static_cast<float>(value);
}

template<std::integral T> [[nodiscard]] constexpr auto toFloatNarrow(T value) noexcept -> float
{
    // Narrowing: some ImGui/ImPlot APIs store or accept values as float.
    return static_cast<float>(value);
}

} // namespace UI::Numeric
