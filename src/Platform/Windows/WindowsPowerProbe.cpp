#include "WindowsPowerProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace Platform
{

WindowsPowerProbe::WindowsPowerProbe()
{
    // Probe capabilities at construction time
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) != 0)
    {
        // Check if there's a battery
        m_Capabilities.hasBattery = (sps.BatteryFlag != 128); // 128 = no system battery
        m_Capabilities.hasChargePercent = m_Capabilities.hasBattery && (sps.BatteryLifePercent <= 100);
        m_Capabilities.hasTimeEstimates = m_Capabilities.hasBattery && (sps.BatteryLifeTime != 0xFFFFFFFF);

        // Windows API provides limited info compared to Linux
        m_Capabilities.hasChargeCapacity = false;
        m_Capabilities.hasPowerRate = false;
        m_Capabilities.hasVoltage = false;
        m_Capabilities.hasTechnology = false;
        m_Capabilities.hasCycleCount = false;
        m_Capabilities.hasHealthPercent = false;
    }
    else
    {
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
        m_Capabilities.hasBattery = false;
    }

    spdlog::debug("WindowsPowerProbe: hasBattery={}", m_Capabilities.hasBattery);
}

PowerCounters WindowsPowerProbe::read()
{
    PowerCounters counters;

    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) == 0)
    {
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
        counters.state = BatteryState::Unknown;
        return counters;
    }

    // Check if battery is present
    if (sps.BatteryFlag == 128) // No system battery
    {
        counters.state = BatteryState::NotPresent;
        counters.isOnAc = true;
        return counters;
    }

    // Parse AC line status
    counters.isOnAc = (sps.ACLineStatus == 1);

    // Parse battery state
    if (sps.BatteryFlag == 255) // Unknown status
    {
        counters.state = BatteryState::Unknown;
    }
    else if (sps.BatteryFlag & 8) // Charging
    {
        counters.state = BatteryState::Charging;
    }
    else if (counters.isOnAc && sps.BatteryLifePercent == 100)
    {
        counters.state = BatteryState::Full;
    }
    else
    {
        counters.state = BatteryState::Discharging;
    }

    // Battery charge percentage (0-100, or 255 for unknown)
    if (sps.BatteryLifePercent <= 100)
    {
        counters.chargePercent = static_cast<int>(sps.BatteryLifePercent);
    }
    else
    {
        counters.chargePercent = -1;
    }

    // Time remaining in seconds
    // BatteryLifeTime: seconds of battery life remaining (0xFFFFFFFF = unknown)
    // BatteryFullLifeTime: seconds when fully charged (0xFFFFFFFF = unknown)
    if (sps.BatteryLifeTime != 0xFFFFFFFF)
    {
        if (counters.state == BatteryState::Discharging)
        {
            counters.timeToEmptySec = sps.BatteryLifeTime;
        }
        else if (counters.state == BatteryState::Charging && sps.BatteryFullLifeTime != 0xFFFFFFFF && sps.BatteryLifeTime < sps.BatteryFullLifeTime)
        {
            counters.timeToFullSec = sps.BatteryFullLifeTime - sps.BatteryLifeTime;
        }
    }

    return counters;
}

PowerCapabilities WindowsPowerProbe::capabilities() const
{
    return m_Capabilities;
}

} // namespace Platform
