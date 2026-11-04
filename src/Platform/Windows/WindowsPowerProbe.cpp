#include "WindowsPowerProbe.h"

#include "Domain/SamplingConfig.h"
#include "Domain/Worker.h"
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
#include <stop_token>
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
    : m_BatteryApi(batteryApi),
      m_GetSystemPowerStatus(getSystemPowerStatus),
      // At most one re-read is queued or running: requestBatteryInfoRefresh() defers a change that comes
      // while one runs until its result is taken. The read itself -- SetupAPI's enumeration and the battery
      // IOCTLs, which a slow ACPI call can stall -- is the worker's alone (#2731); the sampling pass never
      // waits for it. A std::exception from it is logged and reads as no facts, as before; anything else
      // the Worker contains (#3343): it ends Failed, and read() keeps the facts it has.
      m_BatteryInfoWorker({.threadName = BATTERY_READ_THREAD_NAME, .capacity = 1, .admission = Domain::Admission::Refuse},
                          [this](BatteryInfoReread& /*job*/, const std::stop_token& /*stop*/, BatteryInfoWorker::JobContext& context)
                          {
                              Windows::BatteryInfo info;
                              try
                              {
                                  info = Windows::readBatteryInfo(m_BatteryApi);
                              }
                              catch (const std::exception& e)
                              {
                                  spdlog::warn("WindowsPowerProbe: the battery read failed: {}", e.what());
                              }
                              context.finish(std::move(info));
                          })
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
    // The worker's thread starts with the first re-read. Nothing else is queued (one re-read at a time),
    // so a submit that isn't Queued is a worker that has ended (Closed), or no memory to queue it
    // (Refused): either way no result is coming, and the next due re-read tries again.
    const Domain::SubmitResult submitted = m_BatteryInfoWorker.submit(BatteryInfoReread{});
    m_BatteryInfoReadAt = std::chrono::steady_clock::now();
    m_BatteryInfoRefreshing = submitted == Domain::SubmitResult::Queued;
    if (m_BatteryInfoWorker.state() == Domain::WorkerState::Failed)
    {
        reportBatteryInfoWorkerFailed(); // no thread could be started
    }
}

void WindowsPowerProbe::takeFinishedBatteryInfo()
{
    if (!m_BatteryInfoRefreshing)
    {
        return;
    }
    const auto published = m_BatteryInfoWorker.latest();
    if (published->version == m_TakenBatteryInfoVersion)
    {
        if (m_BatteryInfoWorker.state() == Domain::WorkerState::Failed)
        {
            // The re-read threw something the body doesn't catch: no result is coming
            m_BatteryInfoRefreshing = false;
            m_BatteryInfoRefreshAgain = false;
            reportBatteryInfoWorkerFailed();
        }
        return; // Still reading
    }
    m_TakenBatteryInfoVersion = published->version;
    m_BatteryInfo = published->value;
    m_BatteryInfoRefreshing = false;
    spdlog::debug("WindowsPowerProbe: battery facts re-read: found={} relative={}", m_BatteryInfo.found, m_BatteryInfo.relativeCapacity);
    if (m_BatteryInfoRefreshAgain)
    {
        m_BatteryInfoRefreshAgain = false;
        startBatteryInfoRefresh();
    }
}

void WindowsPowerProbe::reportBatteryInfoWorkerFailed()
{
    if (m_BatteryInfoFailureReported)
    {
        return;
    }
    m_BatteryInfoFailureReported = true;
    spdlog::warn("WindowsPowerProbe: the battery re-reads have stopped ({}); the battery facts read so far are kept",
                 m_BatteryInfoWorker.failure());
}

void WindowsPowerProbe::updateCapabilities(const SYSTEM_POWER_STATUS& status)
{
    const Windows::BatteryInfo& info = m_BatteryInfo;
    m_Capabilities = powerCapabilitiesFrom(status.BatteryFlag,
                                           status.BatteryLifePercent,
                                           status.BatteryLifeTime,
                                           BatteryDetailsReported{
                                               .found = info.found,
                                               .designWh = info.designWh,
                                               .fullChargeWh = info.fullChargeWh,
                                               .healthPercent = info.healthPercent,
                                               .cycleCount = info.cycleCount,
                                               .hasTechnology = !info.technology.empty(),
                                           });
}

} // namespace Platform
