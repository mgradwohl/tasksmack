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

WindowsPowerProbe::WindowsPowerProbe()
{
    // Probe capabilities at construction time
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) != 0)
    {
        // Check if there's a battery. Must agree with parsePowerStatus()'s handling of
        // BATTERY_FLAG_UNKNOWN (0xFF): that sentinel reports BatteryState::Unknown from read(),
        // not NotPresent, so hasBattery must not be false for it either - otherwise callers
        // would see hasBattery == false alongside a non-NotPresent state, which violates the
        // power-probe contract.
        m_Capabilities.hasBattery = hasBatteryFromFlag(sps.BatteryFlag);
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
