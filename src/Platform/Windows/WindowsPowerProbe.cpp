#include "WindowsPowerProbe.h"

#include "Platform/PowerTypes.h"
#include "WindowsBatteryInfo.h"
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

#include <chrono>

namespace Platform
{

namespace
{

// How long the battery's static facts are kept before they are read again. A change of power
// source also re-reads them: the full-charge capacity is recalibrated across a charge cycle.
constexpr std::chrono::minutes BATTERY_INFO_REFRESH{5};

} // namespace

WindowsPowerProbe::WindowsPowerProbe() : WindowsPowerProbe(Windows::BatteryDeviceFunctions{})
{}

WindowsPowerProbe::WindowsPowerProbe(const Windows::BatteryDeviceFunctions& batteryApi) : m_BatteryApi(batteryApi)
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

        // GetSystemPowerStatus has no live capacity, rate or voltage
        m_Capabilities.hasChargeCapacity = false;
        m_Capabilities.hasPowerRate = false;
        m_Capabilities.hasVoltage = false;
    }
    else
    {
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
        m_Capabilities.hasBattery = false;
    }

    // The battery device's static facts (#1523). Each is reported only when the battery gave it:
    // relative-unit capacities are not Wh, and a cycle count of 0 is the driver's "no counter".
    refreshBatteryInfo(sps.ACLineStatus == 1);
    const Windows::BatteryInfo& info = m_BatteryInfo;
    m_Capabilities.hasDesignCapacity = info.found && info.designWh > 0.0 && info.fullChargeWh > 0.0;
    m_Capabilities.hasHealthPercent = info.found && info.healthPercent >= 0;
    m_Capabilities.hasCycleCount = info.found && info.cycleCount > 0;
    m_Capabilities.hasTechnology = info.found && !info.technology.empty();

    spdlog::debug(
        "WindowsPowerProbe: hasBattery={} batteryInfo={} relative={}", m_Capabilities.hasBattery, info.found, info.relativeCapacity);
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

    PowerCounters counters = parsePowerStatus(sps.ACLineStatus, sps.BatteryFlag, sps.BatteryLifePercent, sps.BatteryLifeTime);
    if (counters.state == BatteryState::NotPresent)
    {
        return counters;
    }

    if (counters.isOnAc != m_BatteryInfoOnAc || std::chrono::steady_clock::now() - m_BatteryInfoReadAt >= BATTERY_INFO_REFRESH)
    {
        refreshBatteryInfo(counters.isOnAc);
    }
    const Windows::BatteryInfo& info = m_BatteryInfo;
    counters.chargeDesignWh = info.designWh;
    counters.chargeFullWh = info.fullChargeWh;
    counters.healthPercent = info.healthPercent;
    counters.cycleCount = info.cycleCount;
    counters.technology = info.technology;
    counters.manufacturer = info.manufacturer;
    counters.model = info.model;
    return counters;
}

PowerCapabilities WindowsPowerProbe::capabilities() const
{
    return m_Capabilities;
}

void WindowsPowerProbe::refreshBatteryInfo(bool isOnAc)
{
    m_BatteryInfo = Windows::readBatteryInfo(m_BatteryApi);
    m_BatteryInfoReadAt = std::chrono::steady_clock::now();
    m_BatteryInfoOnAc = isOnAc;
}

} // namespace Platform
