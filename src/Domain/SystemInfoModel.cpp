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
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
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
    m_CanReadAuthorized.store(m_Capabilities.canReadAuthorized, std::memory_order_relaxed);
}

void SystemInfoModel::read(const std::stop_token& stopToken)
{
    const std::scoped_lock lock(m_ReadMutex);

    auto next = std::make_shared<SystemInfoSnapshot>();
    // Stamped when the read starts, not when it ends. The crash cache orders publications by its own
    // ticket, taken here, so a slow read that started earlier can't replace one that started later
    // (#1773); the wall-clock time is only shown, since it can step backwards (#2157).
    const std::uint64_t crashReadOrder = m_CrashHistory ? m_CrashHistory->beginRead() : 0;
    const auto started = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    next->readAtUnixSeconds = started > 0 ? static_cast<std::uint64_t>(started) : 0;
    if (m_AuthorizationRequested.exchange(false, std::memory_order_relaxed) && m_CanReadAuthorized.load(std::memory_order_relaxed))
    {
        // The password prompt, on this (the read's) thread; a dismissed prompt just leaves the facts marked
        static_cast<void>(m_Probe->authorize(stopToken));
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
    // The helper can take "Authenticate to read" back during the reads above, once it has shown it runs
    // as another account (Windows, #2769): from this read on the page stops offering it.
    m_CanReadAuthorized.store(m_Probe->canReadAuthorized(), std::memory_order_relaxed);
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

namespace
{

/// @p name without the " #2" that made it unique on the page (LinuxSensors), so a device that only
/// changed its place among its namesakes keeps its range and history (#2495).
[[nodiscard]] std::string_view sensorDriverName(std::string_view name)
{
    const std::size_t hash = name.rfind(" #");
    if (hash == std::string_view::npos || hash + 2 == name.size())
    {
        return name;
    }
    const std::string_view number = name.substr(hash + 2);
    return std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; }) ? name.substr(0, hash) : name;
}

} // namespace

void SystemInfoModel::forgetSensorDevice(std::string_view deviceKey)
{
    const std::string prefix = std::format("{}\x1F", deviceKey); // sensorKey()'s device part
    const auto inDevice = [&prefix](const auto& entry)
    {
        return entry.first.starts_with(prefix);
    };
    std::erase_if(m_SensorRanges, inDevice);
    std::erase_if(m_SensorHistories, inDevice);
}

void SystemInfoModel::addToSensorRanges(const Platform::SensorsInfo& sensors)
{
    if (sensors.listed)
    {
        // A device gone from a full list takes its ranges and histories with it, so one the kernel later
        // gives its hwmon number starts fresh, and the maps don't keep every device ever seen (#2495).
        for (auto known = m_SensorDeviceNames.begin(); known != m_SensorDeviceNames.end();)
        {
            const std::string_view deviceKey = known->first;
            if (std::ranges::any_of(sensors.devices,
                                    [deviceKey](const Platform::SensorDevice& device) { return sensorDeviceKey(device) == deviceKey; }))
            {
                ++known;
                continue;
            }
            forgetSensorDevice(deviceKey);
            known = m_SensorDeviceNames.erase(known);
        }
    }
    const double nowSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const Platform::SensorDevice& device : sensors.devices)
    {
        // A key that now names another driver is another device, swapped in between two reads (#2495).
        const std::string_view deviceKey = sensorDeviceKey(device);
        const std::string_view driverName = sensorDriverName(device.name);
        if (const auto known = m_SensorDeviceNames.find(deviceKey); known == m_SensorDeviceNames.end())
        {
            m_SensorDeviceNames.emplace(deviceKey, driverName);
        }
        else if (known->second != driverName)
        {
            forgetSensorDevice(deviceKey);
            known->second = driverName;
        }
        for (const Platform::SensorReading& reading : device.readings)
        {
            const std::string key = sensorKey(device, reading);
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

std::optional<std::string> SystemInfoModel::tryRead(const std::stop_token& stopToken)
{
    return tryReading([this, &stopToken] { read(stopToken); });
}

} // namespace Domain
