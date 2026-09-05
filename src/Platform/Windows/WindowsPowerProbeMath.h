#pragma once

#include "Platform/PowerTypes.h"

#include <cstdint>

namespace Platform
{

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
    // Note: BATTERY_FLAG_NO_BATTERY (0x80), BATTERY_FLAG_UNKNOWN (0xFF), and
    // BATTERY_FLAG_CHARGING (0x08) are defined in the Windows SDK (winbase.h)
    constexpr std::uint8_t BATTERY_FLAG_NO_BATTERY_BIT = 0x80;
    constexpr std::uint8_t BATTERY_FLAG_UNKNOWN_VALUE = 0xFF;
    constexpr std::uint8_t BATTERY_FLAG_CHARGING_BIT = 0x08;
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

} // namespace Platform
