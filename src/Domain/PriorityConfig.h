#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <string_view>
#include <utility>

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

/// A platform priority class (Windows), lowest first; None where the platform has none (Linux), or
/// the class was not read, and nice is the priority. Mirrors Platform::PriorityClass, which
/// ProcessModel translates into it (#1280).
enum class PriorityClass : std::uint8_t
{
    None,
    Idle,
    BelowNormal,
    Normal,
    AboveNormal,
    High,
    Realtime,
};

/// The class's name, as Process Details and the Processes table spell it: getPriorityLabel()'s
/// labels for the five classes it covers, and "Realtime", which it cannot tell from High. Empty for
/// None.
[[nodiscard]] constexpr std::string_view getPriorityClassLabel(PriorityClass priorityClass) noexcept
{
    switch (priorityClass)
    {
    case PriorityClass::Idle:
        return "Idle";
    case PriorityClass::BelowNormal:
        return "Below Normal";
    case PriorityClass::Normal:
        return "Normal";
    case PriorityClass::AboveNormal:
        return "Above Normal";
    case PriorityClass::High:
        return "High";
    case PriorityClass::Realtime:
        return "Realtime";
    case PriorityClass::None:
    default:
        return {};
    }
}

/// A process's priority label: its class's name where the platform reported one, else the label
/// for its nice value (#1280).
[[nodiscard]] constexpr std::string_view getProcessPriorityLabel(PriorityClass priorityClass, int32_t nice)
{
    return priorityClass == PriorityClass::None ? getPriorityLabel(nice) : getPriorityClassLabel(priorityClass);
}

/// Every label getProcessPriorityLabel() can return, for sizing the column that shows them.
inline constexpr std::array<std::string_view, 6> PROCESS_PRIORITY_LABELS = {
    getPriorityClassLabel(PriorityClass::Realtime),
    getPriorityClassLabel(PriorityClass::High),
    getPriorityClassLabel(PriorityClass::AboveNormal),
    getPriorityClassLabel(PriorityClass::Normal),
    getPriorityClassLabel(PriorityClass::BelowNormal),
    getPriorityClassLabel(PriorityClass::Idle),
};

/// Sort key for a process's priority, smallest = highest priority, as nice orders: by class first
/// (Realtime before High, whatever nice values stand for them), then by nice. Processes with no
/// class (all of them on Linux) all share the first component, so nice alone orders them.
[[nodiscard]] constexpr std::pair<int32_t, int32_t> prioritySortKey(PriorityClass priorityClass, int32_t nice) noexcept
{
    return {-static_cast<int32_t>(priorityClass), nice};
}

} // namespace Domain::Priority
