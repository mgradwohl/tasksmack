#pragma once

// The pure part of the Windows GPU sensors read from D3DKMT adapter performance data (#2586): what
// the kernel graphics adapter reports (D3DKMT_ADAPTER_PERFDATA, D3DKMT_ADAPTER_PERFDATACAPS and
// D3DKMT_NODE_PERFDATA, the figures Task Manager's GPU page draws) as plain integers, which sensors a
// reading says the adapter has, and how a reading fills a GPUCounters. No Windows header, so this is
// built and tested on every platform; D3DKMTAdapterPerfData.h makes the calls.

#include "Platform/GPUTypes.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace Platform
{

/// One adapter's performance data as its kernel driver reports it, unconverted. A field the driver
/// doesn't fill reads 0: WDDM has no validity mask, and a driver that reports no temperature or fan
/// leaves the field at 0 (so does an integrated GPU without a fan; Intel's iGPU driver reports no
/// temperature at all, as Task Manager's GPU page shows). A real reading of 0 (a GPU at 0 °C, a fan
/// stopped with no known maximum) can't be told apart from that, and counts as "no sensor". The 3D
/// engine's clock is the exception: a driver that reports clocks fills the engine's maximum, and the
/// current frequency reads 0 Hz while the engine is power-gated (an idle Intel iGPU in RC6), a real
/// reading rather than a missing one.
struct AdapterPerfReading
{
    std::uint32_t temperatureDeciCelsius = 0; ///< D3DKMT_ADAPTER_PERFDATA::Temperature, tenths of a degree Celsius
    std::uint32_t fanRpm = 0;                 ///< D3DKMT_ADAPTER_PERFDATA::FanRPM
    std::uint32_t maxFanRpm = 0;              ///< D3DKMT_ADAPTER_PERFDATACAPS::MaxFanRPM; 0 when that query failed
    std::uint64_t engineFrequencyHz = 0;      ///< D3DKMT_NODE_PERFDATA::Frequency of node 0, the 3D engine (0 Hz: power-gated)
    std::uint64_t maxEngineFrequencyHz = 0;   ///< D3DKMT_NODE_PERFDATA::MaxFrequency of node 0; 0 when that query failed
    /// D3DKMT_ADAPTER_PERFDATA::Power: the adapter's draw in tenths of a percent of its maximum, not
    /// watts. GPUCounters carries power in watts (the GPU tab draws "12.3 W of 150 W"), so this has no
    /// home there yet and is kept only for a future percent-scaled reading.
    std::uint32_t powerTenthsPercent = 0;
    /// D3DKMT_ADAPTER_PERFDATA::MemoryFrequency and MaxMemoryFrequency, in hertz. GPUCounters has the
    /// core clock only, so these have no home yet either.
    std::uint64_t memoryFrequencyHz = 0;
    std::uint64_t maxMemoryFrequencyHz = 0;
};

/// Tenths of a degree Celsius to whole degrees, rounded half up: 455 is 46 °C, 454 is 45 °C.
[[nodiscard]] constexpr std::int32_t deciCelsiusToCelsius(std::uint32_t deciCelsius) noexcept
{
    constexpr std::uint32_t TENTHS_PER_DEGREE = 10;
    return static_cast<std::int32_t>((deciCelsius + (TENTHS_PER_DEGREE / 2)) / TENTHS_PER_DEGREE);
}

/// Hertz to whole megahertz, rounded half up, saturating at the largest std::uint32_t rather than
/// wrapping for a frequency no GPU has.
[[nodiscard]] constexpr std::uint32_t hertzToMegahertz(std::uint64_t hertz) noexcept
{
    constexpr std::uint64_t HERTZ_PER_MEGAHERTZ = 1'000'000;
    constexpr std::uint64_t HALF_MEGAHERTZ = HERTZ_PER_MEGAHERTZ / 2;
    constexpr std::uint32_t MAX_MEGAHERTZ = std::numeric_limits<std::uint32_t>::max();
    if (hertz > std::numeric_limits<std::uint64_t>::max() - HALF_MEGAHERTZ)
    {
        return MAX_MEGAHERTZ; // Rounding would wrap
    }
    const std::uint64_t megahertz = (hertz + HALF_MEGAHERTZ) / HERTZ_PER_MEGAHERTZ;
    return megahertz > MAX_MEGAHERTZ ? MAX_MEGAHERTZ : static_cast<std::uint32_t>(megahertz);
}

/// The sensors an adapter has, decided from one reading taken at enumeration: temperature when the
/// driver reports one, a clock when node 0 reports its maximum frequency (the current one is 0 Hz
/// while the engine is power-gated, so it can't decide), and a fan when the driver reports a maximum
/// fan speed (the percentage GPUSnapshot draws needs it; a fan at rest then reads 0 %, a real
/// reading). Power and the memory clock are never claimed (see AdapterPerfReading). A failed read
/// (nullopt) means no sensors.
[[nodiscard]] constexpr GPUCapabilities adapterPerfSensorCapabilities(const std::optional<AdapterPerfReading>& reading) noexcept
{
    GPUCapabilities sensors{};
    if (!reading.has_value())
    {
        return sensors;
    }
    sensors.hasTemperature = reading->temperatureDeciCelsius > 0;
    sensors.hasClockSpeeds = reading->maxEngineFrequencyHz > 0;
    sensors.hasFanSpeed = reading->maxFanRpm > 0;
    return sensors;
}

/// Whether @p sensors has any sensor D3DKMT adapter perf data can supply.
[[nodiscard]] constexpr bool hasAnyAdapterPerfSensor(const GPUCapabilities& sensors) noexcept
{
    return sensors.hasTemperature || sensors.hasClockSpeeds || sensors.hasFanSpeed;
}

/// Fill @p counter's temperature, clock and fan from this sample's @p reading, for the sensors
/// @p sensors says the adapter has (adapterPerfSensorCapabilities() at enumeration). A sensor the
/// adapter doesn't have is left unread whatever the sample says, so a field that reads non-zero once
/// doesn't draw a series the adapter never claimed; a temperature that reads 0 this sample, a clock
/// whose node query failed (no maximum), a fan whose maximum didn't come, or a failed read (nullopt),
/// is unavailable this sample -- a gap and an N/A bar, never a real-looking 0 (#1111). A clock of 0 Hz
/// with the maximum read is the engine power-gated: a real 0 MHz. The fan's percentage is Domain's,
/// from the raw speed and maximum (GPUModel::computeSnapshot).
inline void
applyAdapterPerfReading(GPUCounters& counter, const std::optional<AdapterPerfReading>& reading, const GPUCapabilities& sensors) noexcept
{
    const bool temperatureRead = sensors.hasTemperature && reading.has_value() && reading->temperatureDeciCelsius > 0;
    counter.temperatureAvailable = temperatureRead;
    counter.temperatureC = temperatureRead ? deciCelsiusToCelsius(reading->temperatureDeciCelsius) : 0;

    const bool clockRead = sensors.hasClockSpeeds && reading.has_value() && reading->maxEngineFrequencyHz > 0;
    counter.gpuClockAvailable = clockRead;
    counter.gpuClockMHz = clockRead ? hertzToMegahertz(reading->engineFrequencyHz) : 0;

    const bool fanRead = sensors.hasFanSpeed && reading.has_value() && reading->maxFanRpm > 0;
    counter.fanSpeedRaw = fanRead ? reading->fanRpm : 0;
    counter.fanSpeedMaxRaw = fanRead ? reading->maxFanRpm : 0;
}

} // namespace Platform
