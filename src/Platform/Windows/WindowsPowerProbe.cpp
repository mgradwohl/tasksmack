#include "WindowsPowerProbe.h"

#include "Domain/SamplingConfig.h"
#include "Platform/PowerTypes.h"
#include "Platform/ThreadName.h"
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
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace Platform
{

namespace
{

// How long the battery's static facts are kept before they are read again. A change of power
// source also re-reads them: the full-charge capacity is recalibrated across a charge cycle.
constexpr std::chrono::seconds BATTERY_INFO_REFRESH{Domain::Sampling::BATTERY_INFO_REFRESH_SECONDS};

} // namespace

WindowsPowerProbe::WindowsPowerProbe() : WindowsPowerProbe(Windows::BatteryDeviceFunctions{})
{}

WindowsPowerProbe::WindowsPowerProbe(const Windows::BatteryDeviceFunctions& batteryApi, GetSystemPowerStatusFn getSystemPowerStatus)
    : m_BatteryApi(batteryApi), m_GetSystemPowerStatus(getSystemPowerStatus)
{
    SYSTEM_POWER_STATUS sps{};
    const bool statusRead = m_GetSystemPowerStatus(&sps) != 0;
    if (!statusRead)
    {
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
    }

    // The battery device's static facts (#1523), read here once as the seed; read() has a worker read
    // them again when they may have changed (#2731)
    m_BatteryInfo = Windows::readBatteryInfo(m_BatteryApi);
    m_BatteryInfoReadAt = std::chrono::steady_clock::now();
    m_BatteryInfoOnAc = sps.ACLineStatus == 1;
    if (statusRead)
    {
        updateCapabilities(sps);
    }
    // Otherwise no battery is reported until a read() succeeds

    spdlog::debug("WindowsPowerProbe: hasBattery={} batteryInfo={} relative={}",
                  m_Capabilities.hasBattery,
                  m_BatteryInfo.found,
                  m_BatteryInfo.relativeCapacity);
}

PowerCounters WindowsPowerProbe::read()
{
    SYSTEM_POWER_STATUS sps{};
    if (m_GetSystemPowerStatus(&sps) == 0)
    {
        // The capabilities are kept: a failed call says nothing about the battery
        spdlog::warn("WindowsPowerProbe: GetSystemPowerStatus failed");
        PowerCounters counters;
        counters.state = BatteryState::Unknown;
        return counters;
    }

    PowerCounters counters = parsePowerStatus(sps.ACLineStatus, sps.BatteryFlag, sps.BatteryLifePercent, sps.BatteryLifeTime);
    takeFinishedBatteryInfo();
    if (counters.state != BatteryState::NotPresent)
    {
        // A battery absent until now (inserted after start, #2413) is read at once, not at the next
        // periodic refresh
        const bool batteryAppeared = !m_Capabilities.hasBattery;
        if (batteryAppeared || counters.isOnAc != m_BatteryInfoOnAc ||
            std::chrono::steady_clock::now() - m_BatteryInfoReadAt >= BATTERY_INFO_REFRESH)
        {
            requestBatteryInfoRefresh(counters.isOnAc);
        }
    }
    // The capabilities follow every read, as LinuxPowerProbe's do (#2380, #2413): a battery inserted
    // or removed, or details the battery device first answered after start
    updateCapabilities(sps);
    if (counters.state == BatteryState::NotPresent)
    {
        return counters;
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

void WindowsPowerProbe::requestBatteryInfoRefresh(bool isOnAc)
{
    m_BatteryInfoOnAc = isOnAc;
    if (m_BatteryInfoRefreshing)
    {
        m_BatteryInfoRefreshAgain = true;
        return;
    }
    startBatteryInfoRefresh();
}

void WindowsPowerProbe::startBatteryInfoRefresh()
{
    // Assigning joins the previous worker, which has handed its result over and is exiting. The read
    // itself -- SetupAPI's enumeration and the battery IOCTLs, which a slow ACPI call can stall -- is
    // the worker's alone (#2731); the sampling pass never waits for it.
    m_BatteryInfoWorker = std::jthread(
        [this]
        {
            setCurrentThreadName(BATTERY_READ_THREAD_NAME);
            Windows::BatteryInfo info;
            try
            {
                info = Windows::readBatteryInfo(m_BatteryApi);
            }
            catch (const std::exception& e)
            {
                spdlog::warn("WindowsPowerProbe: the battery read failed: {}", e.what());
            }
            const std::scoped_lock lock(m_FinishedBatteryInfoMutex);
            m_FinishedBatteryInfo = std::move(info);
        });
    m_BatteryInfoReadAt = std::chrono::steady_clock::now();
    m_BatteryInfoRefreshing = true;
}

void WindowsPowerProbe::takeFinishedBatteryInfo()
{
    if (!m_BatteryInfoRefreshing)
    {
        return;
    }
    std::optional<Windows::BatteryInfo> finished;
    {
        const std::scoped_lock lock(m_FinishedBatteryInfoMutex);
        finished.swap(m_FinishedBatteryInfo);
    }
    if (!finished.has_value())
    {
        return; // Still reading
    }
    m_BatteryInfo = std::move(*finished);
    m_BatteryInfoRefreshing = false;
    spdlog::debug("WindowsPowerProbe: battery facts re-read: found={} relative={}", m_BatteryInfo.found, m_BatteryInfo.relativeCapacity);
    if (m_BatteryInfoRefreshAgain)
    {
        m_BatteryInfoRefreshAgain = false;
        startBatteryInfoRefresh();
    }
}

void WindowsPowerProbe::updateCapabilities(const SYSTEM_POWER_STATUS& status)
{
    const Windows::BatteryInfo& info = m_BatteryInfo;
    m_Capabilities = powerCapabilitiesFrom(status.BatteryFlag,
                                           status.BatteryLifePercent,
                                           status.BatteryLifeTime,
                                           BatteryDetailsReported{.found = info.found,
                                                                  .designWh = info.designWh,
                                                                  .fullChargeWh = info.fullChargeWh,
                                                                  .healthPercent = info.healthPercent,
                                                                  .cycleCount = info.cycleCount,
                                                                  .hasTechnology = !info.technology.empty()});
}

} // namespace Platform
