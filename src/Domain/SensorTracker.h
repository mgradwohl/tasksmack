#pragma once

// Each sensor's range and recent history (#1522), kept across reads: the bookkeeping the System tab's
// reads and the sensor sampler (#2737) share. Plain state, no locking: its owner serialises it.

#include "Domain/SamplingConfig.h"
#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Domain
{

/// The lowest and highest one sensor has read since the first read (#1522), or since its device was
/// plugged in (#2495).
struct SensorRange
{
    double min = 0.0;
    double max = 0.0;
    std::uint32_t samples = 0; ///< How many reads it was in.

    [[nodiscard]] bool operator==(const SensorRange&) const = default;
};

/// One sensor's recent readings, for its row's chart (#1522), oldest first, `values` aligned with
/// `times`: none more than the history window (the Settings history duration, #2796) older than the
/// newest, and at most Sampling::historyCapacityForSeconds(window) of them. A NaN value marks a break
/// where the reads paused.
struct SensorHistory
{
    std::vector<float> values;
    std::vector<double> times; ///< When each was read, steady_clock seconds (UI::Widgets::historyFrameNowSeconds()'s clock)

    [[nodiscard]] bool operator==(const SensorHistory&) const = default;
};

/// The interval sensors were read at before they followed the Settings interval (#1522).
inline constexpr double SENSOR_DEFAULT_INTERVAL_SECONDS = static_cast<double>(Sampling::SYSTEM_INFO_SENSOR_REFRESH_MS) / 1000.0;

/// Appends @p value read at @p nowSeconds to @p history, read every @p intervalSeconds: a NaN break
/// first when the reads paused (sensorHistoryGapSeconds()), then the oldest dropped past
/// @p windowSeconds (clamped as Sampling::clampHistorySeconds() clamps it) or past
/// Sampling::historyCapacityForSeconds() of it. A time not after the newest is ignored, so the history
/// stays in order.
void appendSensorHistory(SensorHistory& history,
                         double value,
                         double nowSeconds,
                         double intervalSeconds = SENSOR_DEFAULT_INTERVAL_SECONDS,
                         double windowSeconds = Sampling::HISTORY_SECONDS_DEFAULT);

/// A pause longer than this many sensor intervals breaks a sensor's history line...
inline constexpr double SENSOR_HISTORY_GAP_INTERVALS = 3.0;
/// ...and never a shorter one than this (three of the 2 s reads before #2737), so a sampler backing off
/// after an overrun, or the first read after a shorter interval is chosen, doesn't break every point.
inline constexpr double SENSOR_HISTORY_GAP_MIN_SECONDS = 3.0 * SENSOR_DEFAULT_INTERVAL_SECONDS;

/// How long a pause breaks the history line of sensors read every @p intervalSeconds.
[[nodiscard]] constexpr double sensorHistoryGapSeconds(double intervalSeconds) noexcept
{
    const double intervals = SENSOR_HISTORY_GAP_INTERVALS * intervalSeconds;
    return intervals > SENSOR_HISTORY_GAP_MIN_SECONDS ? intervals : SENSOR_HISTORY_GAP_MIN_SECONDS;
}

/// The device part of sensorKey(): SensorDevice::id, or its name from a source without ids.
[[nodiscard]] inline std::string_view sensorDeviceKey(const Platform::SensorDevice& device)
{
    return device.id.empty() ? std::string_view(device.name) : std::string_view(device.id);
}

/// A sensor's key in SensorTracker::ranges() and histories(): its device's and its own
/// identity (SensorDevice::id, SensorReading::id), never its label, so two channels that share a label
/// (two "acpitz" zones, a voltage and a current both "VDD_IN") keep their own range and history (#2340).
/// From a source without ids, the device's name and the reading's kind and label stand in.
[[nodiscard]] inline std::string sensorKey(const Platform::SensorDevice& device, const Platform::SensorReading& reading)
{
    const std::string_view deviceId = sensorDeviceKey(device);
    if (!reading.id.empty())
    {
        return std::format("{}\x1F{}", deviceId, reading.id);
    }
    return std::format("{}\x1F{}\x1F{}", deviceId, std::to_underlying(reading.kind), reading.label);
}

/// Each sensor's range and history over the reads so far, by sensorKey().
class SensorTracker
{
  public:
    /// Adds @p sensors, read at @p nowSeconds (steady_clock seconds), to each sensor's range and history,
    /// first dropping what was kept for a device that is gone or whose key another has taken (#2495).
    /// @p intervalSeconds is how often they are read, which places the histories' breaks, and
    /// @p windowSeconds how far back they reach (appendSensorHistory()).
    void add(const Platform::SensorsInfo& sensors,
             double nowSeconds,
             double intervalSeconds = SENSOR_DEFAULT_INTERVAL_SECONDS,
             double windowSeconds = Sampling::HISTORY_SECONDS_DEFAULT);

    [[nodiscard]] const std::map<std::string, SensorRange>& ranges() const noexcept
    {
        return m_Ranges;
    }

    [[nodiscard]] const std::map<std::string, SensorHistory>& histories() const noexcept
    {
        return m_Histories;
    }

  private:
    /// Drops the ranges and histories of the device keyed @p deviceKey.
    void forgetDevice(std::string_view deviceKey);

    std::map<std::string, SensorRange> m_Ranges;
    std::map<std::string, SensorHistory> m_Histories;
    /// Each device's driver name (without the " #2" that makes it unique on the page) by its
    /// sensorDeviceKey(), as last read: a new name under the same key is a new device (#2495).
    std::map<std::string, std::string, std::less<>> m_DeviceNames;
};

} // namespace Domain
