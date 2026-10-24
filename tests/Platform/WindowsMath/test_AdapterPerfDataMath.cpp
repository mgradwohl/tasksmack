/// @file test_AdapterPerfDataMath.cpp
/// @brief Unit tests for AdapterPerfDataMath.h (#2586): which sensors a D3DKMT adapter perf-data
/// reading says an adapter has, and how a sample's reading fills a GPUCounters -- the temperature's
/// tenths of a degree and the clock's hertz scaled, a sensor the adapter never claimed masked, a
/// temperature that reads 0 or a failed read unavailable this sample, a power-gated engine's 0 Hz a
/// real 0 MHz.
///
/// The header includes no Windows header, so these build and run on every platform; the calls
/// themselves are tested against fakes in test_WindowsD3DKMTAdapterPerfData.cpp.

#include "Platform/GPUTypes.h"
#include "Platform/Windows/AdapterPerfDataMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <optional>

namespace Platform
{
namespace
{

constexpr std::uint64_t MAX_ENGINE_HZ = 2'250'000'000ULL;

TEST(AdapterPerfDataMathTest, DeciCelsiusRoundsHalfUpToWholeDegrees)
{
    EXPECT_EQ(deciCelsiusToCelsius(0), 0);
    EXPECT_EQ(deciCelsiusToCelsius(454), 45);
    EXPECT_EQ(deciCelsiusToCelsius(455), 46);
    EXPECT_EQ(deciCelsiusToCelsius(1000), 100);
}

TEST(AdapterPerfDataMathTest, HertzRoundsHalfUpToWholeMegahertzAndSaturates)
{
    EXPECT_EQ(hertzToMegahertz(0), 0U);
    EXPECT_EQ(hertzToMegahertz(1'850'000'000ULL), 1850U);
    EXPECT_EQ(hertzToMegahertz(1'850'499'999ULL), 1850U);
    EXPECT_EQ(hertzToMegahertz(1'850'500'000ULL), 1851U);
    EXPECT_EQ(hertzToMegahertz(std::numeric_limits<std::uint64_t>::max()), std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(hertzToMegahertz(std::numeric_limits<std::uint64_t>::max() - 400'000), std::numeric_limits<std::uint32_t>::max());
}

// A reading with every field set claims temperature, clock and fan, and no more: power is in tenths
// of a percent, not watts, and the memory clock has no home.
TEST(AdapterPerfDataMathTest, AFullReadingClaimsTemperatureClockAndFanOnly)
{
    const AdapterPerfReading reading{.temperatureDeciCelsius = 612,
                                     .fanRpm = 1200,
                                     .maxFanRpm = 3000,
                                     .engineFrequencyHz = 2'100'000'000ULL,
                                     .maxEngineFrequencyHz = MAX_ENGINE_HZ,
                                     .powerTenthsPercent = 450,
                                     .memoryFrequencyHz = 1'000'000'000ULL,
                                     .maxMemoryFrequencyHz = 1'000'000'000ULL};
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(reading);
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds);
    EXPECT_TRUE(sensors.hasFanSpeed);
    EXPECT_FALSE(sensors.hasPowerMetrics);
    EXPECT_FALSE(sensors.hasEncoderDecoder);
    EXPECT_FALSE(sensors.hasEngineUtilization);
    EXPECT_FALSE(sensors.hasPerProcessMetrics);
    EXPECT_TRUE(hasAnyAdapterPerfSensor(sensors));
}

// An integrated GPU as its driver reports it: a clock range, no temperature and no fan maximum (and so
// no fan). Zero fields are unsupported, not readings of 0 -- except the engine's current frequency,
// which is 0 Hz while it is power-gated, so the maximum decides the clock.
TEST(AdapterPerfDataMathTest, ZeroFieldsAreNoSensorExceptAParkedClock)
{
    const AdapterPerfReading idleIntegrated{.engineFrequencyHz = 0, .maxEngineFrequencyHz = MAX_ENGINE_HZ};
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(idleIntegrated);
    EXPECT_FALSE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds) << "The driver reports the engine's clock range";
    EXPECT_FALSE(sensors.hasFanSpeed) << "A fan speed without a maximum can't be drawn as a percentage";

    // A current frequency without a maximum is a node query that didn't answer: no clock.
    const AdapterPerfReading frequencyWithoutMax{.engineFrequencyHz = 450'000'000ULL};
    EXPECT_FALSE(adapterPerfSensorCapabilities(frequencyWithoutMax).hasClockSpeeds);

    // A fan spinning with its maximum unknown is still no fan sensor.
    const AdapterPerfReading fanWithoutMax{.fanRpm = 900};
    EXPECT_FALSE(adapterPerfSensorCapabilities(fanWithoutMax).hasFanSpeed);
    EXPECT_FALSE(hasAnyAdapterPerfSensor(adapterPerfSensorCapabilities(fanWithoutMax)));

    // A fan at rest with its maximum known is one: 0 RPM is a reading.
    const AdapterPerfReading fanAtRest{.fanRpm = 0, .maxFanRpm = 3000};
    EXPECT_TRUE(adapterPerfSensorCapabilities(fanAtRest).hasFanSpeed);

    EXPECT_FALSE(hasAnyAdapterPerfSensor(adapterPerfSensorCapabilities(AdapterPerfReading{})));
}

TEST(AdapterPerfDataMathTest, AFailedReadingClaimsNoSensor)
{
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(std::nullopt);
    EXPECT_FALSE(sensors.hasTemperature);
    EXPECT_FALSE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_FALSE(hasAnyAdapterPerfSensor(sensors));
}

// A sample's reading fills the counter, scaled: tenths of a degree to degrees, hertz to megahertz,
// the fan's raw speed and maximum unconverted for Domain to make the percentage of.
TEST(AdapterPerfDataMathTest, AReadingFillsTheCounterScaled)
{
    const AdapterPerfReading reading{.temperatureDeciCelsius = 455,
                                     .fanRpm = 1200,
                                     .maxFanRpm = 3000,
                                     .engineFrequencyHz = 1'850'500'000ULL,
                                     .maxEngineFrequencyHz = MAX_ENGINE_HZ,
                                     .powerTenthsPercent = 450};
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(reading);

    GPUCounters counter{};
    counter.gpuId = "PCI_00:02.0_8086:7D55";
    applyAdapterPerfReading(counter, reading, sensors);

    EXPECT_TRUE(counter.temperatureAvailable);
    EXPECT_EQ(counter.temperatureC, 46);
    EXPECT_TRUE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 1851U);
    EXPECT_EQ(counter.fanSpeedRaw, 1200U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 3000U);
    // Untouched: not this source's.
    EXPECT_TRUE(counter.powerAvailable);
    EXPECT_DOUBLE_EQ(counter.powerDrawWatts, 0.0);
    EXPECT_FALSE(counter.suspended);
    EXPECT_EQ(counter.gpuId, "PCI_00:02.0_8086:7D55");
}

// An engine power-gated this sample reads 0 Hz with its maximum still reported: a real 0 MHz, drawn,
// not a gap.
TEST(AdapterPerfDataMathTest, AParkedEngineIsARealZeroMegahertz)
{
    const GPUCapabilities clockOnly = adapterPerfSensorCapabilities(AdapterPerfReading{.maxEngineFrequencyHz = MAX_ENGINE_HZ});
    GPUCounters counter{};
    counter.gpuClockMHz = 999;
    applyAdapterPerfReading(counter, AdapterPerfReading{.engineFrequencyHz = 0, .maxEngineFrequencyHz = MAX_ENGINE_HZ}, clockOnly);
    EXPECT_TRUE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 0U);
    EXPECT_FALSE(counter.temperatureAvailable);
}

// The sensors decided at enumeration mask later samples: an adapter that claimed no fan or clock
// doesn't get one from a sample that reports them, so no series appears mid-run that the adapter
// never claimed (and the GPU tab, which draws by the enumeration's sensors, never sees a value with
// no capability).
TEST(AdapterPerfDataMathTest, ASensorTheAdapterNeverClaimedIsMasked)
{
    const GPUCapabilities temperatureOnly = adapterPerfSensorCapabilities(AdapterPerfReading{.temperatureDeciCelsius = 400});
    const AdapterPerfReading later{.temperatureDeciCelsius = 500,
                                   .fanRpm = 1500,
                                   .maxFanRpm = 3000,
                                   .engineFrequencyHz = 1'000'000'000ULL,
                                   .maxEngineFrequencyHz = MAX_ENGINE_HZ};

    GPUCounters counter{};
    applyAdapterPerfReading(counter, later, temperatureOnly);

    EXPECT_TRUE(counter.temperatureAvailable);
    EXPECT_EQ(counter.temperatureC, 50);
    EXPECT_FALSE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 0U);
    EXPECT_EQ(counter.fanSpeedRaw, 0U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U) << "No fan reading: Domain's fanSpeedAvailable stays false";
}

// A temperature the adapter has that reads 0 this sample is unavailable this sample (a gap, an N/A
// bar), not a real 0 °C; a clock whose node query didn't answer (no maximum) and a fan whose maximum
// didn't come this sample are gaps too.
TEST(AdapterPerfDataMathTest, AZeroReadingOfAClaimedSensorIsUnavailableThisSample)
{
    const GPUCapabilities all = adapterPerfSensorCapabilities(
        AdapterPerfReading{.temperatureDeciCelsius = 400, .maxFanRpm = 3000, .maxEngineFrequencyHz = MAX_ENGINE_HZ});

    GPUCounters counter{};
    counter.temperatureC = 99; // Stale values are replaced, not left standing
    counter.gpuClockMHz = 999;
    applyAdapterPerfReading(counter, AdapterPerfReading{.fanRpm = 1200, .engineFrequencyHz = 450'000'000ULL}, all);

    EXPECT_FALSE(counter.temperatureAvailable);
    EXPECT_EQ(counter.temperatureC, 0);
    EXPECT_FALSE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 0U);
    EXPECT_EQ(counter.fanSpeedRaw, 0U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U);
}

TEST(AdapterPerfDataMathTest, AFailedReadLeavesEverySensorUnavailable)
{
    const GPUCapabilities all = adapterPerfSensorCapabilities(
        AdapterPerfReading{.temperatureDeciCelsius = 400, .maxFanRpm = 3000, .maxEngineFrequencyHz = MAX_ENGINE_HZ});

    GPUCounters counter{};
    counter.temperatureC = 40;
    counter.gpuClockMHz = 1000;
    counter.fanSpeedRaw = 1000;
    counter.fanSpeedMaxRaw = 3000;
    applyAdapterPerfReading(counter, std::nullopt, all);

    EXPECT_FALSE(counter.temperatureAvailable);
    EXPECT_EQ(counter.temperatureC, 0);
    EXPECT_FALSE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 0U);
    EXPECT_EQ(counter.fanSpeedRaw, 0U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U);
}

// An adapter with no perf-data sensors at all: nothing claimed, so every sample leaves its sensors
// unavailable whatever it says.
TEST(AdapterPerfDataMathTest, NoSensorsMeansNothingIsEverFilled)
{
    GPUCounters counter{};
    applyAdapterPerfReading(
        counter,
        AdapterPerfReading{.temperatureDeciCelsius = 500, .maxFanRpm = 100, .engineFrequencyHz = 5, .maxEngineFrequencyHz = 5},
        GPUCapabilities{});
    EXPECT_FALSE(counter.temperatureAvailable);
    EXPECT_FALSE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U);
}

} // namespace
} // namespace Platform
