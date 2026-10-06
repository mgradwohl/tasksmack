#pragma once

// The one mapping between a process's kernel-style state code and the display name ProcessModel
// publishes in ProcessSnapshot::displayState. Both directions read the same table, so the State
// column's letter can't drift from the name Process Details shows (#1352: the column used to take
// the name's first letter, so Stopped read S and Dead read D). Pure and header-only, following
// CONTRIBUTING.md's "extract the pure decision logic into a small header" pattern.

#include <array>
#include <string_view>

namespace Domain
{

/// A kernel-style state code and the display name ProcessModel gives it. Linux probes report these
/// codes from /proc/[pid]/stat; the Windows probe derives R/S/T/I/? from thread states (#1302).
struct ProcessStateName
{
    char code;
    std::string_view name;
};

/// Every state ProcessModel names. Anything else is UNKNOWN_PROCESS_STATE.
inline constexpr std::array<ProcessStateName, 8> PROCESS_STATE_NAMES{{
    {.code = 'R', .name = "Running"},
    {.code = 'S', .name = "Sleeping"},
    {.code = 'D', .name = "Disk Sleep"},
    {.code = 'Z', .name = "Zombie"},
    {.code = 'T', .name = "Stopped"},
    {.code = 't', .name = "Tracing"},
    {.code = 'X', .name = "Dead"},
    {.code = 'I', .name = "Idle"},
}};

/// The code and name for any state not in PROCESS_STATE_NAMES.
inline constexpr ProcessStateName UNKNOWN_PROCESS_STATE{.code = '?', .name = "Unknown"};

/// The display name for a raw state code: 'T' -> "Stopped", anything unlisted -> "Unknown".
[[nodiscard]] constexpr std::string_view processStateName(char code) noexcept
{
    for (const auto& entry : PROCESS_STATE_NAMES)
    {
        if (entry.code == code)
        {
            return entry.name;
        }
    }
    return UNKNOWN_PROCESS_STATE.name;
}

/// The kernel-style code for a display name: "Stopped" -> 'T', "Dead" -> 'X', anything else -> '?'.
[[nodiscard]] constexpr char processStateCode(std::string_view displayState) noexcept
{
    for (const auto& entry : PROCESS_STATE_NAMES)
    {
        if (entry.name == displayState)
        {
            return entry.code;
        }
    }
    return UNKNOWN_PROCESS_STATE.code;
}

} // namespace Domain
