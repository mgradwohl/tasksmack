#pragma once

#include "Platform/PowerTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace Platform
{

// Note: BATTERY_FLAG_NO_BATTERY (0x80), BATTERY_FLAG_UNKNOWN (0xFF), and BATTERY_FLAG_CHARGING
// (0x08) are defined in the Windows SDK (winbase.h). Shared by hasBatteryFromFlag() and
// parsePowerStatus() below so both agree on what BatteryFlag means.
inline constexpr std::uint8_t BATTERY_FLAG_NO_BATTERY_BIT = 0x80;
inline constexpr std::uint8_t BATTERY_FLAG_UNKNOWN_VALUE = 0xFF;
inline constexpr std::uint8_t BATTERY_FLAG_CHARGING_BIT = 0x08;

/// Does this BatteryFlag value indicate a battery is present? Used for
/// PowerCapabilities::hasBattery, so it must stay consistent with parsePowerStatus() below:
/// BATTERY_FLAG_UNKNOWN (0xFF) is a distinct sentinel that also happens to have the
/// BATTERY_FLAG_NO_BATTERY bit (0x80) set, but parsePowerStatus() reports it as
/// BatteryState::Unknown, not NotPresent - so hasBattery must not be false here, or the
/// power-probe contract (hasBattery == false implies BatteryState::NotPresent) would be
/// violated whenever the OS reports an unknown battery flag.
[[nodiscard]] inline bool hasBatteryFromFlag(std::uint8_t batteryFlag)
{
    if (batteryFlag == BATTERY_FLAG_UNKNOWN_VALUE)
    {
        return true;
    }
    return (batteryFlag & BATTERY_FLAG_NO_BATTERY_BIT) == 0;
}

/// Pure parsing of a SYSTEM_POWER_STATUS snapshot into PowerCounters, extracted from
/// WindowsPowerProbe::read() so its battery-state/charge/time-remaining branches can be
/// unit tested with fabricated field values. CI runners have no battery, so these branches
/// never execute against a real GetSystemPowerStatus() result - taking the fields as plain
/// integers (rather than the SYSTEM_POWER_STATUS struct) keeps this header includable from
/// tests without pulling in windows.h.
/// @param acLineStatus SYSTEM_POWER_STATUS::ACLineStatus (1 = on AC)
/// @param batteryFlag SYSTEM_POWER_STATUS::BatteryFlag (BATTERY_FLAG_* bits; 0x80 = no
/// battery, 0xFF = unknown, 0x08 = charging)
/// @param batteryLifePercent SYSTEM_POWER_STATUS::BatteryLifePercent (0-100, or 255 = unknown)
/// @param batteryLifeTimeSec SYSTEM_POWER_STATUS::BatteryLifeTime (seconds, or 0xFFFFFFFF = unknown)
[[nodiscard]] inline PowerCounters
parsePowerStatus(std::uint8_t acLineStatus, std::uint8_t batteryFlag, std::uint8_t batteryLifePercent, std::uint32_t batteryLifeTimeSec)
{
    constexpr std::uint32_t BATTERY_LIFE_TIME_UNKNOWN = 0xFFFFFFFFU;

    PowerCounters counters;

    // BATTERY_FLAG_UNKNOWN (0xFF) is a distinct sentinel value, not a bitmask - it must be
    // checked before the NO_BATTERY bit test below, since 0xFF also has that bit set and
    // would otherwise always be misreported as NotPresent instead of Unknown.
    if (batteryFlag == BATTERY_FLAG_UNKNOWN_VALUE)
    {
        counters.state = BatteryState::Unknown;
    }
    // Check if battery is present
    else if ((batteryFlag & BATTERY_FLAG_NO_BATTERY_BIT) != 0)
    {
        counters.state = BatteryState::NotPresent;
        counters.isOnAc = true;
        return counters;
    }
    else if ((batteryFlag & BATTERY_FLAG_CHARGING_BIT) != 0)
    {
        counters.state = BatteryState::Charging;
    }
    else if (batteryLifePercent == 100)
    {
        // Battery is at 100% - consider it full regardless of AC status
        counters.state = BatteryState::Full;
    }
    else if (acLineStatus == 1)
    {
        // On AC, not charging, below full: held by a charge limit or a paused charger. Linux
        // reports this as "Not charging"; both map to NotCharging, not Discharging (#1158).
        counters.state = BatteryState::NotCharging;
    }
    else
    {
        counters.state = BatteryState::Discharging;
    }

    // Parse AC line status (the NO_BATTERY early return above always forces isOnAc=true instead)
    counters.isOnAc = (acLineStatus == 1);

    // Battery charge percentage (0-100, or 255 for unknown)
    if (batteryLifePercent <= 100)
    {
        counters.chargePercent = static_cast<int>(batteryLifePercent);
    }
    else
    {
        counters.chargePercent = -1;
    }

    // Time remaining in seconds
    // Note: Windows API does not provide time-to-full for charging state
    if (batteryLifeTimeSec != BATTERY_LIFE_TIME_UNKNOWN)
    {
        if (counters.state == BatteryState::Discharging)
        {
            counters.timeToEmptySec = batteryLifeTimeSec;
        }
        // timeToFullSec remains 0 (unavailable) - Windows doesn't provide this
    }

    return counters;
}

// ---- Battery details (#1523): IOCTL_BATTERY_QUERY_INFORMATION's BATTERY_INFORMATION fields ----

/// BATTERY_CAPACITY_RELATIVE (poclass.h): the capacities are relative units, not mWh.
inline constexpr std::uint32_t BATTERY_CAPACITY_RELATIVE_BIT = 0x40000000U;
/// BATTERY_UNKNOWN_CAPACITY (poclass.h)
inline constexpr std::uint32_t BATTERY_CAPACITY_UNKNOWN_VALUE = 0xFFFFFFFFU;

/// BATTERY_INFORMATION::Chemistry, four bytes and not NUL-terminated, as the text Linux's
/// power_supply "technology" uses ("LION" -> "Li-ion"), ignoring case. An unlisted code is returned
/// as reported; one that is blank or not printable ASCII is empty (unknown).
[[nodiscard]] inline std::string chemistryText(std::string_view code)
{
    while (!code.empty() && (code.back() == '\0' || code.back() == ' '))
    {
        code.remove_suffix(1);
    }
    if (code.empty() || !std::ranges::all_of(code, [](char c) { return c > ' ' && c < 0x7F; }))
    {
        return {};
    }
    std::string upper(code);
    std::ranges::transform(upper, upper.begin(), [](char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; });
    constexpr std::array<std::array<std::string_view, 2>, 8> NAMES{{{"LION", "Li-ion"},
                                                                    {"LI-I", "Li-ion"},
                                                                    {"LIP", "Li-poly"},
                                                                    {"PBAC", "Lead-acid"},
                                                                    {"NICD", "NiCd"},
                                                                    {"NIMH", "NiMH"},
                                                                    {"NIZN", "NiZn"},
                                                                    {"RAM", "Alkaline-manganese"}}};
    const auto known = std::ranges::find(NAMES, std::string_view(upper), [](const auto& name) { return name[0]; });
    return known != NAMES.end() ? std::string((*known)[1]) : std::string(code);
}

/// A BATTERY_INFORMATION capacity (mWh) as Wh; 0 when it is not a known mWh figure: relative units
/// (whose scale is the driver's own), zero or BATTERY_UNKNOWN_CAPACITY.
[[nodiscard]] constexpr double capacityWh(std::uint32_t capacity, std::uint32_t capabilities) noexcept
{
    const bool known = (capabilities & BATTERY_CAPACITY_RELATIVE_BIT) == 0 && capacity != BATTERY_CAPACITY_UNKNOWN_VALUE;
    return known ? static_cast<double>(capacity) / 1000.0 : 0.0;
}

/// PowerCounters::healthPercent: full-charge over design capacity, rounded and capped at 100 (a new
/// battery can hold a little more than its design). -1 unless both are known (capacityWh() > 0).
[[nodiscard]] inline int healthPercentFromCapacity(std::uint32_t designed, std::uint32_t fullCharged, std::uint32_t capabilities) noexcept
{
    const double design = capacityWh(designed, capabilities);
    const double full = capacityWh(fullCharged, capabilities);
    if (design <= 0.0 || full <= 0.0)
    {
        return -1;
    }
    return static_cast<int>(std::lround(std::min(full / design, 1.0) * 100.0));
}

} // namespace Platform
