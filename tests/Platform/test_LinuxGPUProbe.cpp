#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GPUTypes.h"
#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/LinuxGPUProbe.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace Platform
{
namespace
{

TEST(LinuxGPUProbeTest, EnumerateReadAndCapabilitiesDoNotThrow)
{
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);

    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto processCounters = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxGPUProbeTest, CounterGpuIdsAreNotEmptyWhenPresent)
{
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);
    const auto counters = probe.readGPUCounters();

    for (const auto& c : counters)
    {
        EXPECT_FALSE(c.gpuId.empty());
    }
}

TEST(LinuxGPUProbeTest, ProcessGpuCountersAreStructurallyValid)
{
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);
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
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasHotspotTemp);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    // None of NVML, DRM, or ROCm provide cumulative PCIe byte counters (NVML/ROCm only expose
    // rates; DRM doesn't expose PCIe throughput at all), so the OR'd composite is always false.
    EXPECT_FALSE(caps.hasPCIeMetrics);
    EXPECT_TRUE(caps.hasEngineUtilization);
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU);
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST(LinuxGPUProbeTest, MockLibrariesContributeEnumeratedGpusAndCounters)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);

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
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);

    const auto processCounters = probe.readProcessGPUCounters();
    const auto merged = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 123; });
    ASSERT_NE(merged, processCounters.end());
    EXPECT_EQ(merged->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(merged->gpuMemoryBytes, 222U);
    EXPECT_EQ(merged->activeEngines.size(), 2U);
}

// #1112: on a hybrid laptop (Intel iGPU + NVIDIA dGPU) the composite capabilities include NVML's
// power and fan, but the Intel adapter's own sensorCapabilities say it has neither (nor, without
// hwmon, a temperature), so the GPU tab doesn't draw those series for it stuck at 0.
TEST(LinuxGPUProbeTest, HybridLaptopIntelGpuHasNoNvmlSensors)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const TestSupport::ScopedTempDir sysRoot("tasksmack_linux_gpu_hybrid");
    const auto pciDir = sysRoot.path / "pci" / "0000:00:02.0";
    std::filesystem::create_directories(pciDir);
    std::filesystem::create_directories(sysRoot.path / "drm" / "card0");
    std::filesystem::create_directory_symlink(pciDir, sysRoot.path / "drm" / "card0" / "device");
    std::filesystem::create_symlink("/nonexistent/drivers/i915", pciDir / "driver");
    std::ofstream(pciDir / "vendor") << "0x8086\n";
    std::ofstream(pciDir / "class") << "0x030000\n";
    std::ofstream(sysRoot.path / "drm" / "card0" / "gt_cur_freq_mhz") << "1100\n";

    LinuxGPUProbe probe((sysRoot.path / "drm").string(), (sysRoot.path / "pci").string());
    const auto caps = probe.capabilities();
    ASSERT_TRUE(caps.hasPowerMetrics);
    ASSERT_TRUE(caps.hasFanSpeed);

    const auto gpus = probe.enumerateGPUs();
    const auto intel = std::ranges::find_if(gpus, [](const GPUInfo& gpu) { return gpu.vendor == "Intel"; });
    ASSERT_NE(intel, gpus.end());
    EXPECT_TRUE(intel->isIntegrated);
    ASSERT_TRUE(intel->sensorCapabilities.has_value());
    const auto intelSensors = intel->sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_FALSE(intelSensors.hasPowerMetrics);
    EXPECT_FALSE(intelSensors.hasFanSpeed);
    EXPECT_FALSE(intelSensors.hasTemperature);
    EXPECT_TRUE(intelSensors.hasClockSpeeds);

    const auto nvidia = std::ranges::find_if(gpus, [](const GPUInfo& gpu) { return gpu.id == "mock-nvml-uuid-0"; });
    ASSERT_NE(nvidia, gpus.end());
    ASSERT_TRUE(nvidia->sensorCapabilities.has_value());
    EXPECT_TRUE(nvidia->sensorCapabilities.value_or(GPUCapabilities{}).hasPowerMetrics);
    EXPECT_TRUE(nvidia->sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);

    // Every adapter has its own sensor set now; none falls back to the OR'd probe capabilities.
    for (const auto& gpu : gpus)
    {
        EXPECT_TRUE(gpu.sensorCapabilities.has_value()) << gpu.id;
    }
}

} // namespace
} // namespace Platform

#endif
