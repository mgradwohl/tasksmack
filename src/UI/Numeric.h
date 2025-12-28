#pragma once

#include "Domain/Numeric.h"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>

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

// Re-export Domain::Numeric::narrowOr for UI layer convenience
using Domain::Numeric::narrowOr;
using Domain::Numeric::toDouble;

// ImPlot series counts are int; keep conversion explicit + checked.
[[nodiscard]] constexpr auto checkedCount(std::size_t value) noexcept -> int
{
    return narrowOr<int>(value, std::numeric_limits<int>::max());
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
