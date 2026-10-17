#include "SystemInfoModel.h"

#include "Domain/CrashHistory.h"
#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>

namespace Domain
{

SystemInfoModel::SystemInfoModel(std::unique_ptr<Platform::ISystemInfoProbe> probe, std::shared_ptr<CrashHistory> crashHistory)
    : m_Probe(std::move(probe)), m_CrashHistory(std::move(crashHistory))
{
    if (!m_Probe)
    {
        throw std::invalid_argument("SystemInfoModel requires a probe");
    }
    m_Capabilities = m_Probe->capabilities();
}

void SystemInfoModel::read()
{
    const std::scoped_lock lock(m_ReadMutex);

    auto next = std::make_shared<SystemInfoSnapshot>();
    // Stamped when the read starts, not when it ends. The crash cache orders publications by its own
    // ticket, taken here, so a slow read that started earlier can't replace one that started later
    // (#1773); the wall-clock time is only shown, since it can step backwards (#2157).
    const std::uint64_t crashReadOrder = m_CrashHistory ? m_CrashHistory->beginRead() : 0;
    const auto started = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    next->readAtUnixSeconds = started > 0 ? static_cast<std::uint64_t>(started) : 0;
    if (m_AuthorizationRequested.exchange(false, std::memory_order_relaxed) && m_Capabilities.canReadAuthorized)
    {
        // The password prompt, on this (the read's) thread; a dismissed prompt just leaves the facts marked
        static_cast<void>(m_Probe->authorize(std::stop_token{}));
    }
    if (m_Capabilities.hasOs)
    {
        next->os = m_Probe->readOs();
        next->firmware = m_Probe->readFirmware();
        next->memory = m_Probe->readMemoryModules();
        next->paging = m_Probe->readCommitPaging();
        next->storage = m_Probe->readStorage();
        next->graphics = m_Probe->readGraphics();
        next->security = m_Probe->readPlatformSecurity();
        next->sensors = m_Probe->readSensors();
        next->devices = m_Probe->readDevices();
        next->drivers = m_Probe->readDrivers();
        next->crashes = m_Probe->readCrashes();
        // The Wi-Fi details that can raise a location prompt only once the user asked for them (#1782).
        next->wifiDetailsIncluded = wifiDetailsIncluded();
        next->adapters = m_Probe->readNetworkAdapters(next->wifiDetailsIncluded);
        next->boot = m_Probe->readBootPerformance();
        // The public addresses go off the machine to ask, so only once the user asked for them (#1518).
        next->publicIpIncluded = m_Capabilities.hasPublicIp && publicIpIncluded();
        if (next->publicIpIncluded)
        {
            next->publicIp = m_Probe->readPublicIp();
        }
    }
    addToSensorRanges(next->sensors);
    next->sensorRanges = m_SensorRanges;
    next->sensorHistories = m_SensorHistories;
    if (m_CrashHistory && m_Capabilities.hasOs)
    {
        // Process Details' crash line (#1675) shows this read too, without a second one of its own.
        m_CrashHistory->publish(next->crashes, next->readAtUnixSeconds, crashReadOrder);
    }
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

namespace
{

/// @p readFn's throw as its message, caught on the thread that threw it (#1685, #1706).
template<typename ReadFn> [[nodiscard]] std::optional<std::string> tryReading(ReadFn&& readFn)
{
    try
    {
        std::forward<ReadFn>(readFn)();
    }
    catch (const std::exception& e)
    {
        return std::string(e.what());
    }
    catch (...)
    {
        return std::string("unknown error");
    }
    return std::nullopt;
}

} // namespace

void appendSensorHistory(SensorHistory& history, double value, double nowSeconds)
{
    if (!history.times.empty() && nowSeconds <= history.times.back())
    {
        return; // in time order only
    }
    constexpr double REFRESH_SECONDS = static_cast<double>(Sampling::SYSTEM_INFO_SENSOR_REFRESH_MS) / 1000.0;
    if (!history.times.empty() && nowSeconds - history.times.back() > SENSOR_HISTORY_GAP_INTERVALS * REFRESH_SECONDS)
    {
        // The reads paused (the tab was hidden): end the line a refresh after its last reading rather
        // than drawing it straight across the pause.
        history.times.push_back(history.times.back() + REFRESH_SECONDS);
        history.values.push_back(std::numeric_limits<float>::quiet_NaN());
    }
    history.times.push_back(nowSeconds);
    history.values.push_back(static_cast<float>(value));

    const double cutoff = nowSeconds - static_cast<double>(Sampling::HISTORY_SECONDS_DEFAULT);
    std::size_t drop =
        history.times.size() > Sampling::SENSOR_HISTORY_CAPACITY ? history.times.size() - Sampling::SENSOR_HISTORY_CAPACITY : 0;
    while (drop < history.times.size() && history.times[drop] < cutoff)
    {
        ++drop;
    }
    const auto count = static_cast<std::ptrdiff_t>(drop);
    history.times.erase(history.times.begin(), history.times.begin() + count);
    history.values.erase(history.values.begin(), history.values.begin() + count);
}

void SystemInfoModel::addToSensorRanges(const Platform::SensorsInfo& sensors)
{
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const Platform::SensorDevice& device : sensors.devices)
    {
        for (const Platform::SensorReading& reading : device.readings)
        {
            const std::string key = sensorRangeKey(device.name, reading.label);
            SensorRange& range = m_SensorRanges[key];
            range.min = range.samples == 0 ? reading.value : std::min(range.min, reading.value);
            range.max = range.samples == 0 ? reading.value : std::max(range.max, reading.value);
            ++range.samples;
            appendSensorHistory(m_SensorHistories[key], reading.value, nowSeconds);
        }
    }
}

void SystemInfoModel::readSensors()
{
    const std::scoped_lock lock(m_ReadMutex);
    if (!m_Capabilities.hasOs || m_LastVersion == 0)
    {
        return; // nothing to keep the other facts of yet
    }
    auto next = std::make_shared<SystemInfoSnapshot>(*m_Slot.load());
    next->sensors = m_Probe->readSensors();
    addToSensorRanges(next->sensors);
    next->sensorRanges = m_SensorRanges;
    next->sensorHistories = m_SensorHistories;
    next->version = ++m_LastVersion;
    m_Slot.commit(std::move(next));
}

std::optional<std::string> SystemInfoModel::tryReadSensors()
{
    return tryReading([this] { readSensors(); });
}

std::optional<std::string> SystemInfoModel::tryRead()
{
    return tryReading([this] { read(); });
}

} // namespace Domain
