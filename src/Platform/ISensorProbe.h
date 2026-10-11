#pragma once

// The per-tick sensor read behind the System Information page's live Sensors section (#1522): the
// same sensors ISystemInfoProbe::readSensors() lists, found once and then read value by value, so the
// Domain can keep each one's minimum, maximum and history since start. Raw values only.

#include "Platform/ISystemInfoProbe.h"

#include <cstddef>
#include <span>

namespace Platform
{

class ISensorProbe
{
  public:
    ISensorProbe() = default;
    virtual ~ISensorProbe() = default;
    ISensorProbe(const ISensorProbe&) = delete;
    ISensorProbe& operator=(const ISensorProbe&) = delete;
    ISensorProbe(ISensorProbe&&) = delete;
    ISensorProbe& operator=(ISensorProbe&&) = delete;

    /// Finds the sensors and reads each once: the facts readSensors() returns, whose readings, device
    /// by device, are the order readValues() then fills. The slow part (directory scans, opening the
    /// files or the counter query); call it once, off the UI thread. A second call finds them again.
    [[nodiscard]] virtual SensorsInfo discover() = 0;

    /// Reads every sensor discover() found into @p values, in the same order and the kind's unit; NaN
    /// for one that couldn't be read this time. @p values holds sensorCount(discover()) entries; any
    /// beyond the sensors found are left alone. Cheap: no enumeration, and no allocation in steady state.
    virtual void readValues(std::span<double> values) = 0;
};

/// The number of readings in @p info: the length of the values readValues() fills.
[[nodiscard]] inline std::size_t sensorCount(const SensorsInfo& info) noexcept
{
    std::size_t count = 0;
    for (const SensorDevice& device : info.devices)
    {
        count += device.readings.size();
    }
    return count;
}

/// The probe for a platform without sensors: finds none.
class UnsupportedSensorProbe final : public ISensorProbe
{
  public:
    [[nodiscard]] SensorsInfo discover() override
    {
        return {};
    }

    void readValues(std::span<double> /*values*/) override
    {}
};

} // namespace Platform
