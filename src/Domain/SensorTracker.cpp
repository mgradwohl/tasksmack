#include "SensorTracker.h"

#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
#include <string>
#include <string_view>

namespace Domain
{

void appendSensorHistory(SensorHistory& history, double value, double nowSeconds, double intervalSeconds, double windowSeconds)
{
    if (!history.times.empty() && nowSeconds <= history.times.back())
    {
        return; // in time order only
    }
    if (!history.times.empty() && nowSeconds - history.times.back() > sensorHistoryGapSeconds(intervalSeconds))
    {
        // The reads paused: end the line an interval after its last reading rather than drawing it
        // straight across the pause.
        history.times.push_back(history.times.back() + intervalSeconds);
        history.values.push_back(std::numeric_limits<float>::quiet_NaN());
    }
    history.times.push_back(nowSeconds);
    history.values.push_back(static_cast<float>(value));

    const double window = Sampling::clampHistorySeconds(windowSeconds);
    const double cutoff = nowSeconds - window;
    const std::size_t capacity = Sampling::historyCapacityForSeconds(window);
    std::size_t drop = history.times.size() > capacity ? history.times.size() - capacity : 0;
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

void SensorTracker::forgetDevice(std::string_view deviceKey)
{
    const std::string prefix = std::format("{}\x1F", deviceKey); // sensorKey()'s device part
    const auto inDevice = [&prefix](const auto& entry)
    {
        return entry.first.starts_with(prefix);
    };
    std::erase_if(m_Ranges, inDevice);
    std::erase_if(m_Histories, inDevice);
}

void SensorTracker::add(const Platform::SensorsInfo& sensors, double nowSeconds, double intervalSeconds, double windowSeconds)
{
    if (sensors.listed)
    {
        // A device gone from a full list takes its ranges and histories with it, so one the kernel later
        // gives its hwmon number starts fresh, and the maps don't keep every device ever seen (#2495).
        for (auto known = m_DeviceNames.begin(); known != m_DeviceNames.end();)
        {
            const std::string_view deviceKey = known->first;
            if (std::ranges::any_of(sensors.devices,
                                    [deviceKey](const Platform::SensorDevice& device) { return sensorDeviceKey(device) == deviceKey; }))
            {
                ++known;
                continue;
            }
            forgetDevice(deviceKey);
            known = m_DeviceNames.erase(known);
        }
    }
    for (const Platform::SensorDevice& device : sensors.devices)
    {
        // A key that now names another driver is another device, swapped in between two reads (#2495).
        const std::string_view deviceKey = sensorDeviceKey(device);
        const std::string_view driverName = sensorDriverName(device.name);
        if (const auto known = m_DeviceNames.find(deviceKey); known == m_DeviceNames.end())
        {
            m_DeviceNames.emplace(deviceKey, driverName);
        }
        else if (known->second != driverName)
        {
            forgetDevice(deviceKey);
            known->second = driverName;
        }
        for (const Platform::SensorReading& reading : device.readings)
        {
            const std::string key = sensorKey(device, reading);
            SensorRange& range = m_Ranges[key];
            range.min = range.samples == 0 ? reading.value : std::min(range.min, reading.value);
            range.max = range.samples == 0 ? reading.value : std::max(range.max, reading.value);
            ++range.samples;
            appendSensorHistory(m_Histories[key], reading.value, nowSeconds, intervalSeconds, windowSeconds);
        }
    }
}

} // namespace Domain
