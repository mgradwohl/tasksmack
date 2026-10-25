#pragma once

// The Windows Sensors facts (#1522), unprivileged: the ACPI thermal zones' temperatures from the PDH
// counter "\Thermal Zone Information(*)\Temperature" (kelvin), as one "Thermal zones" device. That is all
// Windows itself exposes: fans, voltages and most motherboard temperatures need the board vendor's
// driver, and the section says so. Read once per refresh; min/max and charts need a sampled model.
// The PDH calls go through an injectable table; WindowsThermalZones.cpp supplies the real one, tests
// substitute fakes. The rest is standard library only, so its tests run on every platform.

#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
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

/// The thermal zones through @p fns, as one device sorted by label. A zone reading 0 K isn't populated
/// by the firmware and is left out.
inline void readSensors(SensorsInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;
    std::vector<ZoneSample> zones;
    const std::uint32_t status = fns.readZones(zones);
    if (status != 0 && !meansNoZones(status))
    {
        return; // listed stays false: the counters couldn't be read
    }
    info.listed = true;
    SensorDevice device{.name = std::string(DEVICE_NAME), .id = "thermal", .readings = {}};
    for (const ZoneSample& zone : zones)
    {
        if (zone.kelvin <= 0.0)
        {
            continue;
        }
        device.readings.push_back({
            .kind = SensorKind::Temperature,
            .label = zoneLabel(zone.instance),
            .id = zone.instance, // unique, where labels might not be (#2340)
            .value = zone.kelvin - KELVIN_AT_ZERO_CELSIUS,
            .high = {},
            .critical = {},
        });
    }
    if (device.readings.empty())
    {
        return;
    }
    // By label, then identity, so zones that share a label keep one order from read to read.
    std::ranges::sort(device.readings,
                      [](const SensorReading& left, const SensorReading& right)
                      { return std::tie(left.label, left.id) < std::tie(right.label, right.id); });
    info.devices.push_back(std::move(device));
}

} // namespace Platform::WindowsThermalZones
