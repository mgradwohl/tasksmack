#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/ROCmGPUProbe.h"
#include "Platform/Linux/ROCmGPUProbeMath.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Platform
{
namespace
{

// ROCmGPUProbeMath: pure logic exercised with fake function pointers, covering error and
// missing-symbol branches the fixed-behavior mock library (tests/Mocks/ROCmMock.cpp) never
// takes -- it always succeeds, so these paths are otherwise unreachable via ROCmGPUProbe itself.

TEST(ROCmGPUProbeMathTest, ResolveErrorStringReturnsLibraryStringWhenAvailable)
{
    const auto result = ROCmGPUProbeMath::resolveErrorString(7, [](std::uint32_t) -> const char* { return "Input out of bounds"; });
    EXPECT_EQ(result, "Input out of bounds");
}

TEST(ROCmGPUProbeMathTest, ResolveErrorStringFallsBackWhenFunctionPointerIsNull)
{
    EXPECT_EQ(ROCmGPUProbeMath::resolveErrorString(3, nullptr), "Unknown ROCm error 3");
}

TEST(ROCmGPUProbeMathTest, ResolveErrorStringFallsBackWhenLibraryReturnsNull)
{
    const auto result = ROCmGPUProbeMath::resolveErrorString(3, [](std::uint32_t) -> const char* { return nullptr; });
    EXPECT_EQ(result, "Unknown ROCm error 3");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdPrefersUniqueId)
{
    const auto uniqueIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 4001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(0, uniqueIdFn, pciIdFn), "4001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToPciIdWhenUniqueIdUnavailable)
{
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(1, nullptr, pciIdFn), "9001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToPciIdWhenUniqueIdFails)
{
    const auto uniqueIdFn = [](std::uint32_t, std::uint64_t*) -> std::uint32_t
    {
        return ROCmGPUProbeMath::kRsmiStatusSuccess + 1; // any non-success status
    };
    const auto pciIdFn = [](std::uint32_t, std::uint64_t* out) -> std::uint32_t
    {
        *out = 9001;
        return ROCmGPUProbeMath::kRsmiStatusSuccess;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(1, uniqueIdFn, pciIdFn), "9001");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToAmdIndexWhenBothLookupsUnavailable)
{
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(2, nullptr, nullptr), "amd_2");
}

TEST(ROCmGPUProbeMathTest, DeriveDeviceIdFallsBackToAmdIndexWhenBothLookupsFail)
{
    const auto alwaysFails = [](std::uint32_t, std::uint64_t*) -> std::uint32_t
    {
        return ROCmGPUProbeMath::kRsmiStatusSuccess + 1;
    };
    EXPECT_EQ(ROCmGPUProbeMath::deriveDeviceId(2, alwaysFails, alwaysFails), "amd_2");
}

TEST(LinuxROCmGPUProbeTest, BasicOperationsDoNotThrow)
{
    ROCmGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxROCmGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    ROCmGPUProbe probe;
    if (probe.isAvailable())
    {
        // Under CTest (ENVIRONMENT_MODIFICATION), LD_LIBRARY_PATH is prepended with the
        // mock library so the probe is always available. The unavailable path cannot be
        // exercised in this process.
        GTEST_SKIP() << "ROCm probe is available (mock library loaded); unavailable path not testable in this environment";
    }

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasHotspotTemp);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
}

TEST(LinuxROCmGPUProbeTest, ProcessCountersAreEmptyWhenAvailableOrUnavailable)
{
    ROCmGPUProbe probe;
    const auto processCounters = probe.readProcessGPUCounters();
    EXPECT_TRUE(processCounters.empty());
}

TEST(LinuxROCmGPUProbeTest, MockLibraryEnablesAvailableCapabilities)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasHotspotTemp);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPCIeMetrics);
    EXPECT_FALSE(caps.hasEngineUtilization);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.hasEncoderDecoder);
    EXPECT_TRUE(caps.supportsMultiGPU);
}

TEST(LinuxROCmGPUProbeTest, MockLibraryEnumeratesDevicesWithFallbackIdentifiers)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);

    EXPECT_EQ(gpus[0].name, "Mock AMD GPU 0");
    EXPECT_EQ(gpus[0].id, "4001");
    EXPECT_EQ(gpus[0].driverVersion, "ROCm");
    EXPECT_EQ(gpus[0].vendor, "AMD");

    EXPECT_EQ(gpus[1].name, "AMD GPU 1");
    EXPECT_EQ(gpus[1].id, "9001");

    EXPECT_EQ(gpus[2].name, "Mock AMD GPU 2");
    EXPECT_EQ(gpus[2].id, "amd_2");
}

TEST(LinuxROCmGPUProbeTest, MockLibraryReturnsExpectedCountersAndFallbacks)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock ROCm library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    ROCmGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);

    // gpuId must match the ID returned by enumerateGPUs() so the domain layer can correlate
    // counters to their GPUInfo entry. Device 0: uniqueId=4001; device 1: pciId=9001;
    // device 2: no unique/PCI id, falls back to "amd_<index>".
    EXPECT_EQ(counters[0].gpuId, "4001");
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 80.0);
    EXPECT_EQ(counters[0].memoryUsedBytes, 3ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 12ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_EQ(counters[0].hotspotTempC, 72);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 150.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 220.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1500U);
    EXPECT_EQ(counters[0].memoryClockMHz, 2000U);
    // ROCmMock reports a raw fan value of 170 and a mocked RSMI_MAX_FAN_SPEED of 255; ROCmGPUProbe
    // stores both unconverted (Domain normalizes them to a percentage -- see #734).
    EXPECT_EQ(counters[0].fanSpeedRaw, 170U);
    EXPECT_EQ(counters[0].fanSpeedMaxRaw, 255U);

    EXPECT_EQ(counters[1].gpuId, "9001");
    EXPECT_EQ(counters[1].hotspotTempC, -1);
    EXPECT_EQ(counters[1].gpuClockMHz, 0U);
    EXPECT_EQ(counters[1].memoryClockMHz, 0U);
    EXPECT_EQ(counters[1].fanSpeedRaw, 0U); // hasFanSpeed=false in the mock
    EXPECT_EQ(counters[1].fanSpeedMaxRaw, 0U);

    EXPECT_EQ(counters[2].gpuId, "amd_2");
    // Legitimate 0 reading (fan stopped / 0% duty) must still be stored as a real sample --
    // fanSpeedMaxRaw must be populated too, not left at 0 as if the metric were unavailable
    // (regression coverage for a "fanSpeed > 0" guard that used to misclassify this as missing).
    EXPECT_EQ(counters[2].fanSpeedRaw, 0U);
    EXPECT_EQ(counters[2].fanSpeedMaxRaw, 255U);
    EXPECT_EQ(counters[2].pcieTxBytes, 0U);
    EXPECT_EQ(counters[2].pcieRxBytes, 0U);
    EXPECT_DOUBLE_EQ(counters[2].computeUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].encoderUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].decoderUtilPercent, 0.0);
}

} // namespace
} // namespace Platform

#endif
