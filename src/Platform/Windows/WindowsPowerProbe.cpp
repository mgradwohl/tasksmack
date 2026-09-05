#include "WindowsPowerProbe.h"

#include "Platform/PowerTypes.h"
#include "WindowsPowerProbeMath.h"

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

// Note: BATTERY_FLAG_NO_BATTERY (0x80), BATTERY_FLAG_UNKNOWN (0xFF), and
// BATTERY_FLAG_CHARGING (0x08) are defined in the Windows SDK (winbase.h)

WindowsPowerProbe::WindowsPowerProbe()
{
    // Probe capabilities at construction time
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) != 0)
    {
        // Check if there's a battery
        m_Capabilities.hasBattery = (sps.BatteryFlag & BATTERY_FLAG_NO_BATTERY) == 0;
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
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) == 0)
    {
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
        PowerCounters counters;
        counters.state = BatteryState::Unknown;
        return counters;
    }

    return parsePowerStatus(sps.ACLineStatus, sps.BatteryFlag, sps.BatteryLifePercent, sps.BatteryLifeTime);
}

PowerCapabilities WindowsPowerProbe::capabilities() const
{
    return m_Capabilities;
}

} // namespace Platform
