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
#include <string>
#include <utility>
#include <vector>

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
      // while one runs until its result is taken. The read itself -- SetupAPI's enumeration, the opens and
      // the tag and information IOCTLs, which a slow ACPI call can stall -- is the worker's alone (#2731);
      // the sampling pass never waits for it, and sends only IOCTL_BATTERY_QUERY_STATUS to the devices the
      // read left open (#4030). A std::exception from it is logged and reads as no facts, as before; anything else
      // the Worker contains (#3343): it ends Failed, and read() keeps the facts it has.
      m_BatteryInfoWorker({.threadName = BATTERY_READ_THREAD_NAME, .capacity = 1, .admission = Domain::Admission::Refuse},
                          [this](BatteryInfoReread& /*job*/, const std::stop_token& /*stop*/, BatteryInfoWorker::JobContext& context)
                          {
                              Windows::BatteryScan info;
                              try
                              {
                                  info = Windows::scanBatteries(m_BatteryApi);
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
    m_Battery = Windows::scanBatteries(m_BatteryApi);
    m_BatteryInfoReadAt = std::chrono::steady_clock::now();
    m_BatteryInfoOnAc = sps.ACLineStatus == 1;
    if (statusRead)
    {
        if (hasBatteryFromFlag(sps.BatteryFlag))
        {
            // The live status too, so the capabilities offer the rate, charge and voltage from the start (#4030)
            PowerCounters seed;
            readBatteryStatus(seed);
        }
        updateCapabilities(sps);
    }
    // Otherwise no battery is reported until a read() succeeds

    spdlog::debug("WindowsPowerProbe: hasBattery={} batteryInfo={} relative={} devices={}",
                  m_Capabilities.hasBattery,
                  m_Battery.info.found,
                  m_Battery.info.relativeCapacity,
                  m_Battery.devices.size());
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
        // The live rate, charge and voltage, on the devices the last re-read opened (#4030); before the
        // capabilities, which follow what it gave
        readBatteryStatus(counters);
    }
    // The capabilities follow every read, as LinuxPowerProbe's do (#2380, #2413): a battery inserted
    // or removed, or details the battery device first answered after start
    updateCapabilities(sps);
    if (counters.state == BatteryState::NotPresent)
    {
        return counters;
    }

    const Windows::BatteryInfo& info = m_Battery.info;
    counters.chargeDesignWh = info.designWh;
    counters.chargeFullWh = info.fullChargeWh;
    counters.healthPercent = info.healthPercent;
    counters.cycleCount = info.cycleCount;
    counters.technology = info.technology;
    counters.manufacturer = info.manufacturer;
    counters.model = info.model;
    // GetSystemPowerStatus gives no time to full; the rate and the remaining Wh do, as on Linux (#4030)
    if (counters.state == BatteryState::Charging && m_Capabilities.hasChargeCapacity)
    {
        counters.timeToFullSec = timeToFullSeconds(counters.chargeNowWh, counters.chargeFullWh, counters.powerNowW);
    }
    return counters;
}

void WindowsPowerProbe::readBatteryStatus(PowerCounters& counters)
{
    std::vector<BatteryStatusReading> readings;
    readings.reserve(m_Battery.devices.size());
    std::erase_if(m_Battery.devices,
                  [&](const Windows::BatteryDevice& device)
                  {
                      BatteryStatusReading reading;
                      switch (Windows::queryBatteryStatus(m_BatteryApi, device, reading))
                      {
                      case Windows::BatteryStatusOutcome::Read:
                          readings.push_back(reading);
                          return false;
                      case Windows::BatteryStatusOutcome::Failed:
                          return false;
                      case Windows::BatteryStatusOutcome::Stale:
                          // Taken out or swapped: the next re-read opens and tags what is in the slot now. The
                          // worker's last publication still shares the handle until that re-read replaces it.
                          spdlog::debug("WindowsPowerProbe: battery tag {} has gone stale; dropped until the next re-read", device.tag);
                          return true;
                      }
                      return false;
                  });
    if (readings.empty())
    {
        return; // No status this sample: the figures read 0, and what was offered stays offered
    }
    const BatteryStatusTotals totals = combineBatteryStatus(readings);
    counters.powerNowW = totals.powerNowW;
    counters.chargeNowWh = totals.chargeNowWh;
    counters.voltageNowMv = totals.voltageMv;
    m_StatusGaveRate = m_StatusGaveRate || totals.hasPowerRate;
    m_StatusGaveCapacity = m_StatusGaveCapacity || totals.hasChargeCapacity;
    m_StatusGaveVoltage = m_StatusGaveVoltage || totals.hasVoltage;
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
    // All or nothing: a copy that throws leaves the facts, the taken version and m_BatteryInfoRefreshing
    // as they were, so the next read() takes this publication again instead of skipping it for good.
    const auto published = m_BatteryInfoWorker.latest();
    if (!takeNewerPublication(*published, m_Battery, m_TakenBatteryInfoVersion))
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
    m_BatteryInfoRefreshing = false;
    // The new devices' status decides afresh what is offered: a battery swapped for one without a rate
    m_StatusGaveRate = false;
    m_StatusGaveCapacity = false;
    m_StatusGaveVoltage = false;
    spdlog::debug("WindowsPowerProbe: battery facts re-read: found={} relative={} devices={}",
                  m_Battery.info.found,
                  m_Battery.info.relativeCapacity,
                  m_Battery.devices.size());
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
    const std::string reason = m_BatteryInfoWorker.failure(); // may throw: then it is reported next time
    m_BatteryInfoFailureReported = true;
    spdlog::warn("WindowsPowerProbe: the battery re-reads have stopped ({}); the battery facts read so far are kept", reason);
}

void WindowsPowerProbe::updateCapabilities(const SYSTEM_POWER_STATUS& status)
{
    const Windows::BatteryInfo& info = m_Battery.info;
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
                                               .hasPowerRate = m_StatusGaveRate,
                                               .hasChargeCapacity = m_StatusGaveCapacity,
                                               .hasVoltage = m_StatusGaveVoltage,
                                           });
}

} // namespace Platform
