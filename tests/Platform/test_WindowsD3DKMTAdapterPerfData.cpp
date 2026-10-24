/// @file test_WindowsD3DKMTAdapterPerfData.cpp
/// @brief Platform::readAdapterPerfData() (#2586) against fake D3DKMT calls (tests/Mocks/WindowsDXGIFake.h):
/// the adapter perf data, caps and node 0 perf data an adapter answers with reach the reading, the
/// temperature scaling end to end through AdapterPerfDataMath.h, a caps or node query that fails
/// leaves only its fields 0, a failing open or perf-data query is nullopt, and the kernel adapter is
/// closed whatever happens. The real calls need an adapter whose driver reports perf data, so a
/// smoke read of this machine's adapters is in test_WindowsGPUProbe.cpp's probe tests.

#ifdef _WIN32

#include "Mocks/WindowsDXGIFake.h"
#include "Platform/GPUTypes.h"
#include "Platform/Windows/AdapterPerfDataMath.h"
#include "Platform/Windows/D3DKMTAdapterPerfData.h"
#include "Platform/Windows/DXGIAdapterLocation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>

namespace Platform
{
namespace
{

using namespace Platform::DXGIFake; // NOLINT(google-build-using-namespace)

constexpr std::uint32_t VENDOR_INTEL = 0x8086;
constexpr std::uint32_t VENDOR_AMD = 0x1002;
constexpr std::uint32_t LUID_IGPU = 0x100;
constexpr std::uint32_t LUID_DGPU = 0x200;
constexpr std::uint32_t LUID_UNKNOWN = 0x999;

constexpr LUID luidOf(std::uint32_t lowPart)
{
    return LUID{.LowPart = lowPart, .HighPart = 0};
}

class WindowsD3DKMTAdapterPerfDataTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dxgiState() = FakeDXGIState{};
        FakeAdapter igpu = makeAdapter(L"Intel(R) Arc(TM) Graphics",
                                       VENDOR_INTEL,
                                       0x7D55,
                                       LUID_IGPU,
                                       PciLocation{.bus = 0x00, .device = 0x02, .function = 0},
                                       128ULL << 20U);
        igpu.perf =
            FakeAdapterPerf{.temperatureDeciCelsius = 455, .engineFrequencyHz = 1'850'000'000ULL, .maxEngineFrequencyHz = 2'250'000'000ULL};
        FakeAdapter dgpu =
            makeAdapter(L"AMD Radeon RX 7800 XT", VENDOR_AMD, 0x747E, LUID_DGPU, PciLocation{.bus = 0x03, .device = 0x00, .function = 0});
        dgpu.perf = FakeAdapterPerf{.temperatureDeciCelsius = 612,
                                    .fanRpm = 1200,
                                    .powerTenthsPercent = 450,
                                    .memoryFrequencyHz = 2'425'000'000ULL,
                                    .maxMemoryFrequencyHz = 2'425'000'000ULL,
                                    .maxFanRpm = 3300,
                                    .engineFrequencyHz = 2'124'000'000ULL,
                                    .maxEngineFrequencyHz = 2'430'000'000ULL};
        setAdapters({igpu, dgpu});
        m_Functions = fakeD3DKMTFunctions();
    }

    /// The fake adapter with this LUID.
    [[nodiscard]] static FakeAdapter& adapter(std::uint32_t luidLowPart)
    {
        FakeAdapter* found = adapterWithLuid(luidOf(luidLowPart));
        if (found == nullptr)
        {
            throw std::logic_error("no fake adapter with that LUID");
        }
        return *found;
    }

    /// The perf data the fake adapter with this LUID answers with.
    [[nodiscard]] static FakeAdapterPerf& perfOf(std::uint32_t luidLowPart)
    {
        std::optional<FakeAdapterPerf>& perf = adapter(luidLowPart).perf;
        if (!perf.has_value())
        {
            throw std::logic_error("that fake adapter has no perf data");
        }
        return *perf;
    }

    /// The reading for this LUID, which the test expects to succeed; a failed read fails the test.
    [[nodiscard]] AdapterPerfReading read(std::uint32_t luidLowPart) const
    {
        const std::optional<AdapterPerfReading> reading = readAdapterPerfData(luidOf(luidLowPart), m_Functions);
        EXPECT_TRUE(reading.has_value());
        return reading.value_or(AdapterPerfReading{});
    }

    D3DKMTAdapterFunctions m_Functions{};
};

// Every field the three queries answer reaches the reading, unconverted.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, TheAdaptersPerfDataCapsAndNodeClockReachTheReading)
{
    const AdapterPerfReading reading = read(LUID_DGPU);
    EXPECT_EQ(reading.temperatureDeciCelsius, 612U);
    EXPECT_EQ(reading.fanRpm, 1200U);
    EXPECT_EQ(reading.maxFanRpm, 3300U);
    EXPECT_EQ(reading.engineFrequencyHz, 2'124'000'000ULL);
    EXPECT_EQ(reading.maxEngineFrequencyHz, 2'430'000'000ULL);
    EXPECT_EQ(reading.powerTenthsPercent, 450U);
    EXPECT_EQ(reading.memoryFrequencyHz, 2'425'000'000ULL);
    EXPECT_EQ(reading.maxMemoryFrequencyHz, 2'425'000'000ULL);
    EXPECT_EQ(dxgiState().perfQueries, 1);
    EXPECT_EQ(dxgiState().adapterOpens, 1);
}

// Through the math: 45.5 °C reads 46 °C, 1.85 GHz reads 1850 MHz, and the iGPU without a fan
// maximum has temperature and clock but no fan.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, TemperatureAndClockAreScaledIntoTheCounter)
{
    const std::optional<AdapterPerfReading> reading = read(LUID_IGPU);
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(reading);
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasFanSpeed);

    GPUCounters counter{};
    applyAdapterPerfReading(counter, reading, sensors);
    EXPECT_TRUE(counter.temperatureAvailable);
    EXPECT_EQ(counter.temperatureC, 46);
    EXPECT_TRUE(counter.gpuClockAvailable);
    EXPECT_EQ(counter.gpuClockMHz, 1850U);
    EXPECT_EQ(counter.fanSpeedMaxRaw, 0U);
}

// A caps query the driver refuses (no fan maximum) leaves the fan maximum 0 and the rest read.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, AFailedCapsQueryLeavesOnlyTheFanMaximumUnread)
{
    perfOf(LUID_DGPU).capsOk = false;
    const AdapterPerfReading reading = read(LUID_DGPU);
    EXPECT_EQ(reading.temperatureDeciCelsius, 612U);
    EXPECT_EQ(reading.fanRpm, 1200U);
    EXPECT_EQ(reading.maxFanRpm, 0U);
    EXPECT_EQ(reading.engineFrequencyHz, 2'124'000'000ULL);
    EXPECT_FALSE(adapterPerfSensorCapabilities(reading).hasFanSpeed);
}

// A node perf-data query the driver refuses leaves the clock 0: temperature and fan stand.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, AFailedNodeQueryLeavesOnlyTheClockUnread)
{
    perfOf(LUID_DGPU).nodeOk = false;
    const AdapterPerfReading reading = read(LUID_DGPU);
    EXPECT_EQ(reading.engineFrequencyHz, 0ULL);
    EXPECT_EQ(reading.maxEngineFrequencyHz, 0ULL);
    EXPECT_EQ(reading.temperatureDeciCelsius, 612U);
    EXPECT_EQ(reading.maxFanRpm, 3300U);
    const GPUCapabilities sensors = adapterPerfSensorCapabilities(reading);
    EXPECT_FALSE(sensors.hasClockSpeeds);
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasFanSpeed);
}

// An adapter whose driver answers no perf data at all (older than WDDM 2.4, a software adapter):
// the read is nullopt -- no sensor, nothing to log -- and the caps and node queries aren't made.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, AnAdapterWithoutPerfDataIsNullopt)
{
    adapter(LUID_IGPU).perf.reset();
    const std::optional<AdapterPerfReading> reading = readAdapterPerfData(luidOf(LUID_IGPU), m_Functions);
    EXPECT_FALSE(reading.has_value());
    EXPECT_EQ(dxgiState().perfQueries, 1);
    EXPECT_EQ(dxgiState().adapterOpens, 1) << "Opened, refused, and closed";
    EXPECT_FALSE(hasAnyAdapterPerfSensor(adapterPerfSensorCapabilities(reading)));
}

// A LUID no kernel adapter answers to (removed, or re-created under a new LUID after a driver
// reset): the open fails, so nothing is queried.
TEST_F(WindowsD3DKMTAdapterPerfDataTest, AFailingOpenIsNulloptWithoutAQuery)
{
    const std::optional<AdapterPerfReading> reading = readAdapterPerfData(luidOf(LUID_UNKNOWN), m_Functions);
    EXPECT_FALSE(reading.has_value());
    EXPECT_EQ(dxgiState().perfQueries, 0);
    EXPECT_EQ(dxgiState().adapterOpens, 0);
}

// The real gdi32 calls against this machine: a read of a LUID no adapter has fails cleanly, without
// throwing or crashing. (This machine's own adapters are read through WindowsGPUProbe in
// test_WindowsGPUProbe.cpp's smoke tests.)
TEST(WindowsD3DKMTAdapterPerfDataSmokeTest, TheRealCallsHandleAnUnknownLuid)
{
    EXPECT_FALSE(readAdapterPerfData(LUID{.LowPart = 0xDEADBEEF, .HighPart = 0x7FFFFFFF}).has_value());
}

} // namespace
} // namespace Platform

#endif // _WIN32
