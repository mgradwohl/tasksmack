#pragma once

// The Windows Sensors facts (#1522), unprivileged: the ACPI thermal zones' temperatures from the PDH
// counter "\Thermal Zone Information(*)\Temperature" (kelvin), as one "Thermal zones" device. That is all
// Windows itself exposes: fans, voltages and most motherboard temperatures need the board vendor's
// driver, and the section says so. The System Information read lists them once per refresh;
// ThermalZoneSensorProbe reads them per tick for the live section's minimum, maximum and charts.
// The PDH calls go through an injectable table or reader; WindowsThermalZones.cpp supplies the real
// ones, tests substitute fakes. The rest is standard library only, so its tests run on every platform.

#include "Platform/ISensorProbe.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::WindowsThermalZones
{

/// The counter path, in English (PdhAddEnglishCounterW), so a localized Windows finds it too.
inline constexpr std::wstring_view COUNTER_PATH = L"\\Thermal Zone Information(*)\\Temperature";
inline constexpr std::string_view DEVICE_NAME = "Thermal zones";
inline constexpr double KELVIN_AT_ZERO_CELSIUS = 273.15;

// The PDH statuses that mean "no thermal zones" rather than "the counters couldn't be read".
inline constexpr std::uint32_t PDH_CSTATUS_NO_OBJECT_CODE = 0xC0000BB8U;
inline constexpr std::uint32_t PDH_CSTATUS_NO_COUNTER_CODE = 0xC0000BB9U;
inline constexpr std::uint32_t PDH_CSTATUS_NO_INSTANCE_CODE = 0x800007D1U;
inline constexpr std::uint32_t PDH_NO_DATA_CODE = 0x800007D5U;

/// One counter instance's value.
struct ZoneSample
{
    std::string instance; ///< The counter instance, "\_TZ.TZ00"
    double kelvin = 0.0;
};

/// The calls readSensors() makes.
struct Functions
{
    /// Every thermal zone's temperature, from one collection of COUNTER_PATH; the PDH status (0 on success).
    std::uint32_t (*readZones)(std::vector<ZoneSample>& zones) = nullptr;
};

/// The real table, over PDH (WindowsThermalZones.cpp; Windows only).
[[nodiscard]] Functions systemFunctions();

/// A PDH status that means there are no thermal zones to read.
[[nodiscard]] constexpr bool meansNoZones(std::uint32_t status) noexcept
{
    return status == PDH_CSTATUS_NO_OBJECT_CODE || status == PDH_CSTATUS_NO_COUNTER_CODE || status == PDH_CSTATUS_NO_INSTANCE_CODE ||
           status == PDH_NO_DATA_CODE;
}

/// A zone's label: the ACPI name without the "\_TZ." scope ("\_TZ.TZ00" to "TZ00").
[[nodiscard]] inline std::string zoneLabel(std::string_view instance)
{
    if (instance.starts_with('\\'))
    {
        instance.remove_prefix(1);
    }
    if (instance.starts_with("_TZ."))
    {
        instance.remove_prefix(std::string_view("_TZ.").size());
    }
    return instance.empty() ? std::string("Thermal zone") : std::string(instance);
}

/// The thermal zones from one collection, @p zones read with PDH status @p status, as one device sorted
/// by label. A zone reading 0 K isn't populated by the firmware and is left out. With @p instances, each
/// reading's counter instance is stored there, in the readings' order.
inline void
sensorsFromZones(SensorsInfo& info, std::uint32_t status, std::span<const ZoneSample> zones, std::vector<std::string>* instances = nullptr)
{
    info.available = true;
    info.family = OsFamily::Windows;
    if (status != 0 && !meansNoZones(status))
    {
        return; // listed stays false: the counters couldn't be read
    }
    info.listed = true;
    std::vector<std::pair<SensorReading, std::string>> readings;
    for (const ZoneSample& zone : zones)
    {
        if (zone.kelvin <= 0.0)
        {
            continue;
        }
        readings.emplace_back(SensorReading{.kind = SensorKind::Temperature,
                                            .label = zoneLabel(zone.instance),
                                            .value = zone.kelvin - KELVIN_AT_ZERO_CELSIUS,
                                            .high = {},
                                            .critical = {}},
                              zone.instance);
    }
    if (readings.empty())
    {
        return;
    }
    std::ranges::sort(readings, {}, [](const auto& entry) -> const std::string& { return entry.first.label; });
    SensorDevice device{.name = std::string(DEVICE_NAME), .readings = {}};
    device.readings.reserve(readings.size());
    for (auto& [reading, instance] : readings)
    {
        device.readings.push_back(std::move(reading));
        if (instances != nullptr)
        {
            instances->push_back(std::move(instance));
        }
    }
    info.devices.push_back(std::move(device));
}

/// The thermal zones through @p fns: one collection, as sensorsFromZones() lists it.
inline void readSensors(SensorsInfo& info, const Functions& fns)
{
    std::vector<ZoneSample> zones;
    const std::uint32_t status = fns.readZones(zones);
    sensorsFromZones(info, status, zones);
}

/// Each listed zone's temperature in degrees Celsius from one collection: @p values[i] is the zone
/// whose instance is @p instances[i], NaN when this collection didn't have it or it read 0 K. No
/// allocation: a linear match, as a machine has a handful of zones.
inline void zoneValues(std::span<const ZoneSample> zones, std::span<const std::string> instances, std::span<double> values) noexcept
{
    const std::size_t count = std::min(instances.size(), values.size());
    std::size_t index = 0;
    while (index < count)
    {
        values[index] = std::numeric_limits<double>::quiet_NaN();
        for (const ZoneSample& zone : zones)
        {
            if (zone.instance == instances[index] && zone.kelvin > 0.0)
            {
                values[index] = zone.kelvin - KELVIN_AT_ZERO_CELSIUS;
                break;
            }
        }
        ++index;
    }
}

/// One collection of the counter, through a query the reader keeps open between calls: fills the
/// zones (cleared first) and returns the PDH status. The real one is makeZoneReader().
using ZoneReader = std::function<std::uint32_t(std::vector<ZoneSample>& zones)>;

/// The real reader, over one PDH query opened on its first call and kept until the reader goes
/// (WindowsThermalZones.cpp; Windows only). Collecting an open query skips the counter-path parse and
/// the query setup a one-shot read pays every time.
[[nodiscard]] ZoneReader makeZoneReader();

/// The live Sensors section's per-tick read on Windows (#1522): discover() lists the zones as
/// readSensors() does; readValues() collects the kept-open query again and matches each listed zone by
/// its counter instance. The zone vector's capacity is reused: no allocation per tick once the zones'
/// names fit it (short ACPI names stay in std::string's small buffer).
class ThermalZoneSensorProbe final : public ISensorProbe
{
  public:
    explicit ThermalZoneSensorProbe(ZoneReader readZones) : m_ReadZones(std::move(readZones))
    {}

    [[nodiscard]] SensorsInfo discover() override
    {
        SensorsInfo info;
        m_Instances.clear();
        m_Zones.clear();
        const std::uint32_t status = m_ReadZones(m_Zones);
        sensorsFromZones(info, status, m_Zones, &m_Instances);
        return info;
    }

    void readValues(std::span<double> values) override
    {
        if (m_Instances.empty())
        {
            return; // none listed (most desktops and VMs): no counter collection at all
        }
        m_Zones.clear();
        if (m_ReadZones(m_Zones) != 0)
        {
            m_Zones.clear(); // a failed collection: every zone is a gap this time
        }
        zoneValues(m_Zones, m_Instances, values);
    }

  private:
    ZoneReader m_ReadZones;
    std::vector<std::string> m_Instances; ///< discover()'s readings' counter instances, in order
    std::vector<ZoneSample> m_Zones;      ///< The latest collection, its capacity reused
};

} // namespace Platform::WindowsThermalZones
