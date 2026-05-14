#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/LinuxGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace Platform
{
namespace
{

TEST(LinuxGPUProbeTest, EnumerateReadAndCapabilitiesDoNotThrow)
{
    LinuxGPUProbe probe;

    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto processCounters = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxGPUProbeTest, CounterGpuIdsAreNotEmptyWhenPresent)
{
    LinuxGPUProbe probe;
    const auto counters = probe.readGPUCounters();

    for (const auto& c : counters)
    {
        EXPECT_FALSE(c.gpuId.empty());
    }
}

TEST(LinuxGPUProbeTest, ProcessGpuCountersAreStructurallyValid)
{
    LinuxGPUProbe probe;
    const auto processCounters = probe.readProcessGPUCounters();

    for (const auto& c : processCounters)
    {
        EXPECT_GT(c.pid, 0);
        EXPECT_FALSE(c.gpuId.empty());
        EXPECT_GE(c.gpuUtilPercent, 0.0);
    }
}

TEST(LinuxGPUProbeTest, MockLibrariesExposeCompositeCapabilities)
{
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe;

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasHotspotTemp);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasPCIeMetrics);
    EXPECT_TRUE(caps.hasEngineUtilization);
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU);
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST(LinuxGPUProbeTest, MockLibrariesContributeEnumeratedGpusAndCounters)
{
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe;

    const auto gpus = probe.enumerateGPUs();
    EXPECT_NE(std::ranges::find_if(gpus, [](const GPUInfo& gpu) { return gpu.vendor == "NVIDIA" && gpu.id == "mock-nvml-uuid-0"; }),
              gpus.end());
    EXPECT_NE(std::ranges::find_if(gpus, [](const GPUInfo& gpu) { return gpu.vendor == "AMD" && gpu.id == "4001"; }), gpus.end());

    const auto counters = probe.readGPUCounters();
    EXPECT_NE(std::ranges::find_if(
                  counters, [](const GPUCounters& counter) { return counter.gpuId == "mock-nvml-uuid-0" && counter.temperatureC == 65; }),
              counters.end());
    EXPECT_NE(
        std::ranges::find_if(counters, [](const GPUCounters& counter) { return counter.gpuId == "4001" && counter.hotspotTempC == 72; }),
        counters.end());
}

TEST(LinuxGPUProbeTest, MockLibrariesProvidePerProcessCountersFromNvmlProbe)
{
    const auto envGuard = TestSupport::useMockGpuLibrariesWithOverrides();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe;

    const auto processCounters = probe.readProcessGPUCounters();
    const auto merged = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 123; });
    ASSERT_NE(merged, processCounters.end());
    EXPECT_EQ(merged->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(merged->gpuMemoryBytes, 222U);
    EXPECT_EQ(merged->activeEngines.size(), 2U);
}

} // namespace
} // namespace Platform

#endif
