#pragma once

#include <algorithm>
#include <concepts>

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

} // namespace Domain::Numeric
