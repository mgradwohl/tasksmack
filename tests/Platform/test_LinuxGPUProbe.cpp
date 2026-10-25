#if defined(__linux__) && __has_include(<unistd.h>)

#include "App/Panels/GpuSection.h"
#include "Domain/GPUModel.h"
#include "Domain/GPUSnapshot.h"
#include "Platform/GPUTypes.h"
#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/LinuxGPUProbe.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>

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
    const LinuxGPUProbe probe("/sys/class/drm", TestSupport::ISOLATED_PCI_ROOT);

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    EXPECT_TRUE(caps.hasEngineUtilization);
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU);
    EXPECT_TRUE(caps.hasEncoderDecoder); // NVML's, ORed in (#1477)
}

// #1477: NVML's encoder/decoder utilization reaches the GPU tab -- the composite probe's
// capabilities, the adapter's own sensor set, GPUModel's snapshot and history -- for the GPU with
// video engines, and the one without (NVML_ERROR_NOT_SUPPORTED) gets no series at all.
TEST(LinuxGPUProbeTest, NvmlEncoderDecoderUtilizationReachesTheGpuTab)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock GPU libraries not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    Domain::GPUModel model(std::make_unique<LinuxGPUProbe>("/nonexistent/tasksmack/drm", TestSupport::ISOLATED_PCI_ROOT));
    model.refresh();

    const auto publication = model.publication();
    ASSERT_NE(publication, nullptr);
    ASSERT_TRUE(publication->capabilities.hasEncoderDecoder);

    const auto sensorsOf = [&publication](const char* gpuId) -> std::optional<GPUCapabilities>
    {
        const auto info = std::ranges::find_if(publication->gpuInfo, [gpuId](const GPUInfo& gpu) { return gpu.id == gpuId; });
        return info == publication->gpuInfo.end() ? std::nullopt : info->sensorCapabilities;
    };
    EXPECT_TRUE(App::GpuSection::capabilitiesForGpu(publication->capabilities, sensorsOf("mock-nvml-uuid-0")).hasEncoderDecoder);
    EXPECT_FALSE(App::GpuSection::capabilitiesForGpu(publication->capabilities, sensorsOf("nvidia-1")).hasEncoderDecoder);

    const auto snapshot =
        std::ranges::find_if(publication->snapshots, [](const Domain::GPUSnapshot& snap) { return snap.gpuId == "mock-nvml-uuid-0"; });
    ASSERT_NE(snapshot, publication->snapshots.end());
    EXPECT_TRUE(snapshot->encoderAvailable);
    EXPECT_DOUBLE_EQ(snapshot->encoderUtilPercent, 30.0);
    EXPECT_TRUE(snapshot->decoderAvailable);
    EXPECT_DOUBLE_EQ(snapshot->decoderUtilPercent, 12.0);

    const auto& history = publication->histories.at("mock-nvml-uuid-0");
    ASSERT_FALSE(history.encoder.empty());
    EXPECT_FLOAT_EQ(history.encoder.back(), 30.0F);
    ASSERT_FALSE(history.decoder.empty());
    EXPECT_FLOAT_EQ(history.decoder.back(), 12.0F);

    // The GPU without video engines records gaps, not a line at 0%.
    const auto& noEngines = publication->histories.at("nvidia-1");
    ASSERT_FALSE(noEngines.encoder.empty());
    EXPECT_TRUE(std::isnan(noEngines.encoder.back()));
    EXPECT_TRUE(std::isnan(noEngines.decoder.back()));
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
        std::ranges::find_if(counters, [](const GPUCounters& counter) { return counter.gpuId == "4001" && counter.temperatureC == 65; }),
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

// #1116: the composite rescans every vendor probe, including one that had no
// device at startup, and reports the change: here an Intel card appears after
// construction, with no Intel card before.
TEST(LinuxGPUProbeTest, FullRescanPicksUpACardFromAProbeThatHadNone)
{
    const TestSupport::ScopedTempDir sysRoot("tasksmack_linux_gpu_rescan");
    std::filesystem::create_directories(sysRoot.path / "drm");

    LinuxGPUProbe probe((sysRoot.path / "drm").string(), TestSupport::ISOLATED_PCI_ROOT);
    const auto before = probe.enumerateGPUs();
    EXPECT_TRUE(std::ranges::none_of(before, [](const GPUInfo& gpu) { return gpu.vendor == "Intel"; }));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    const auto pciDir = sysRoot.path / "pci" / "0000:00:02.0";
    std::filesystem::create_directories(pciDir);
    std::filesystem::create_directories(sysRoot.path / "drm" / "card0");
    std::filesystem::create_directory_symlink(pciDir, sysRoot.path / "drm" / "card0" / "device");
    std::filesystem::create_symlink("/nonexistent/drivers/i915", pciDir / "driver");
    std::ofstream(pciDir / "vendor") << "0x8086\n";
    std::ofstream(pciDir / "class") << "0x030000\n";

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto after = probe.enumerateGPUs();
    EXPECT_EQ(after.size(), before.size() + 1);
    EXPECT_TRUE(std::ranges::any_of(after, [](const GPUInfo& gpu) { return gpu.id == "0000:00:02.0"; }));
    const auto counters = probe.readGPUCounters();
    EXPECT_TRUE(std::ranges::any_of(counters, [](const GPUCounters& counter) { return counter.gpuId == "0000:00:02.0"; }));
}

} // namespace
} // namespace Platform

#endif
