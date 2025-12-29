#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <string_view>

namespace Domain::Priority
{

// Unix nice value range (-20 = highest priority, 19 = lowest priority)
inline constexpr int32_t MIN_NICE = -20;
inline constexpr int32_t MAX_NICE = 19;
inline constexpr int32_t NORMAL_NICE = 0;

// Priority label thresholds (used by Windows SetPriorityClass and UI display)
// These thresholds define the boundaries between priority classes:
// - nice < -10: High priority
// - -10 <= nice < -5: Above normal priority
// - -5 <= nice < 5: Normal priority
// - 5 <= nice < 15: Below normal priority
// - nice >= 15: Idle priority
inline constexpr int32_t HIGH_THRESHOLD = -10;
inline constexpr int32_t ABOVE_NORMAL_THRESHOLD = -5;
inline constexpr int32_t BELOW_NORMAL_THRESHOLD = 5;
inline constexpr int32_t IDLE_THRESHOLD = 15;

/// Clamp nice value to valid range (-20 to 19)
template<std::integral T> [[nodiscard]] constexpr T clampNice(T value)
{
    return std::clamp(value, static_cast<T>(MIN_NICE), static_cast<T>(MAX_NICE));
}

/// Get human-readable priority label for a nice value
/// @param nice Unix nice value (-20 to 19)
/// @return Priority label ("High", "Above Normal", "Normal", "Below Normal", "Idle")
[[nodiscard]] constexpr std::string_view getPriorityLabel(int32_t nice)
{
    if (nice < HIGH_THRESHOLD)
    {
        return "High";
    }
    if (nice < ABOVE_NORMAL_THRESHOLD)
    {
        return "Above Normal";
    }
    if (nice < BELOW_NORMAL_THRESHOLD)
    {
        return "Normal";
    }
    if (nice < IDLE_THRESHOLD)
    {
        return "Below Normal";
    }
    return "Idle";
}

} // namespace Domain::Priority
