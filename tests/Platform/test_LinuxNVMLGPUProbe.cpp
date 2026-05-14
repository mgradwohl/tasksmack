#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/NVMLGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace Platform
{
namespace
{

TEST(LinuxNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxNVMLGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    NVMLGPUProbe probe;
    if (probe.isAvailable())
    {
        // Under CTest, LD_LIBRARY_PATH points at the mock library so the probe is
        // always available. The unavailable path cannot be exercised in this process.
        GTEST_SKIP() << "NVML probe is available (mock library loaded); unavailable path not testable in this environment";
    }

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPCIeMetrics);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.supportsMultiGPU);
    EXPECT_FALSE(caps.hasEngineUtilization);
}

TEST(LinuxNVMLGPUProbeTest, AvailableProbeReturnsConsistentIds)
{
    NVMLGPUProbe probe;
    if (probe.isAvailable())
    {
        const auto gpus = probe.enumerateGPUs();
        const auto counters = probe.readGPUCounters();
        for (const auto& gpu : gpus)
        {
            EXPECT_FALSE(gpu.id.empty());
        }
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty());
        }
    }
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryEnablesAvailableCapabilities)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasPCIeMetrics);
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU);
    EXPECT_TRUE(caps.hasEngineUtilization);
    EXPECT_FALSE(caps.hasHotspotTemp);
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryEnumeratesDevicesAndUsesUuidFallback)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);

    EXPECT_EQ(gpus[0].vendor, "NVIDIA");
    EXPECT_EQ(gpus[0].name, "Mock NVIDIA GPU 0");
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_FALSE(gpus[0].isIntegrated);
    EXPECT_EQ(gpus[0].deviceIndex, 0U);

    EXPECT_EQ(gpus[1].vendor, "NVIDIA");
    EXPECT_EQ(gpus[1].name, "Mock NVIDIA GPU 1");
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    EXPECT_FALSE(gpus[1].isIntegrated);
    EXPECT_EQ(gpus[1].deviceIndex, 1U);
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryReturnsExpectedCountersAndMergesProcessEngines)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);

    EXPECT_EQ(counters[0].gpuId, "mock-nvml-uuid-0");
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 75.0);
    EXPECT_EQ(counters[0].memoryUsedBytes, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 8ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 125.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 250.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1800U);
    EXPECT_EQ(counters[0].memoryClockMHz, 9000U);
    EXPECT_EQ(counters[0].fanSpeedRPMPercent, 40U);
    EXPECT_EQ(counters[0].pcieTxBytes, 32U * 1024U);
    EXPECT_EQ(counters[0].pcieRxBytes, 64U * 1024U);

    EXPECT_EQ(counters[1].gpuId, "nvidia-1");
    EXPECT_DOUBLE_EQ(counters[1].utilizationPercent, 25.0);

    const auto processCounters = probe.readProcessGPUCounters();
    ASSERT_EQ(processCounters.size(), 2U);

    const auto merged = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 123; });
    ASSERT_NE(merged, processCounters.end());
    EXPECT_EQ(merged->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(merged->gpuMemoryBytes, 222U);
    ASSERT_EQ(merged->activeEngines.size(), 2U);
    EXPECT_EQ(merged->activeEngines[0], "Compute");
    EXPECT_EQ(merged->activeEngines[1], "3D");

    const auto graphicsOnly = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 456; });
    ASSERT_NE(graphicsOnly, processCounters.end());
    EXPECT_EQ(graphicsOnly->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(graphicsOnly->gpuMemoryBytes, 333U);
    ASSERT_EQ(graphicsOnly->activeEngines.size(), 1U);
    EXPECT_EQ(graphicsOnly->activeEngines[0], "3D");
}

} // namespace
} // namespace Platform

#endif
