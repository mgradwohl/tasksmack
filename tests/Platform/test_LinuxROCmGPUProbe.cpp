#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/ROCmGPUProbe.h"

#include <gtest/gtest.h>

namespace Platform
{
namespace
{

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
    if (!probe.isAvailable())
    {
        const auto caps = probe.capabilities();
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasHotspotTemp);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasClockSpeeds);
        EXPECT_FALSE(caps.hasFanSpeed);
        EXPECT_FALSE(caps.hasPerProcessMetrics);
    }
}

TEST(LinuxROCmGPUProbeTest, ProcessCountersAreEmptyWhenAvailableOrUnavailable)
{
    ROCmGPUProbe probe;
    const auto processCounters = probe.readProcessGPUCounters();
    EXPECT_TRUE(processCounters.empty());
}

TEST(LinuxROCmGPUProbeTest, MockLibraryEnablesAvailableCapabilities)
{
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
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
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
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
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
    ROCmGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);

    EXPECT_EQ(counters[0].gpuId, "0");
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 80.0);
    EXPECT_EQ(counters[0].memoryUsedBytes, 3ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 12ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_EQ(counters[0].hotspotTempC, 72);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 150.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 220.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1500U);
    EXPECT_EQ(counters[0].memoryClockMHz, 2000U);
    EXPECT_EQ(counters[0].fanSpeedRPMPercent, 1700U);

    EXPECT_EQ(counters[1].gpuId, "1");
    EXPECT_EQ(counters[1].hotspotTempC, -1);
    EXPECT_EQ(counters[1].gpuClockMHz, 0U);
    EXPECT_EQ(counters[1].memoryClockMHz, 0U);
    EXPECT_EQ(counters[1].fanSpeedRPMPercent, 0U);

    EXPECT_EQ(counters[2].gpuId, "2");
    EXPECT_EQ(counters[2].pcieTxBytes, 0U);
    EXPECT_EQ(counters[2].pcieRxBytes, 0U);
    EXPECT_DOUBLE_EQ(counters[2].computeUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].encoderUtilPercent, 0.0);
    EXPECT_DOUBLE_EQ(counters[2].decoderUtilPercent, 0.0);
}

} // namespace
} // namespace Platform

#endif
