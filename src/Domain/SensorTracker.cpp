#include "SensorTracker.h"

#include "Domain/SamplingConfig.h"
#include "Domain/SharedHistory.h"
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

void appendSensorHistory(SensorSeries& series, double value, double nowSeconds, double intervalSeconds, double windowSeconds)
{
    SharedHistoryBuffer<double>& times = series.m_Times;
    SharedHistoryBuffer<float>& values = series.m_Values;
    if (!times.empty() && nowSeconds <= times.latest())
    {
        return; // in time order only
    }
    const double window = Sampling::clampHistorySeconds(windowSeconds);
    const std::size_t capacity = Sampling::historyCapacityForSeconds(window);
    times.setCapacity(capacity);
    values.setCapacity(capacity);
    const bool paused = !times.empty() && nowSeconds - times.latest() > sensorHistoryGapSeconds(intervalSeconds);
    // Room for both appends in both series first, so a std::bad_alloc leaves them aligned (#1412).
    const std::size_t appends = paused ? 2 : 1;
    times.reserve(appends);
    values.reserve(appends);
    if (paused)
    {
        // The reads paused: end the line an interval after its last reading rather than drawing it
        // straight across the pause.
        times.push(times.latest() + intervalSeconds);
        values.push(std::numeric_limits<float>::quiet_NaN());
    }
    times.push(nowSeconds);
    values.push(static_cast<float>(value));

    // Past the capacity the pushes dropped the oldest; past the window, drop those too. The newest is
    // never older than the cutoff, so at least it stays.
    const double cutoff = nowSeconds - window;
    std::size_t drop = 0;
    while (drop < times.size() && times.ref(drop) < cutoff)
    {
        ++drop;
    }
    times.discardFront(drop);
    values.discardFront(drop);
}

std::map<std::string, SensorHistory> SensorTracker::histories() const
{
    std::map<std::string, SensorHistory> views;
    for (const auto& entry : m_Histories)
    {
        views.emplace_hint(views.end(), entry.first, entry.second.view());
    }
    return views;
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
