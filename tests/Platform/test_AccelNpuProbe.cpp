/// @file test_AccelNpuProbe.cpp
/// @brief NPUs and compute accelerators on Linux (#1827), from a fixture /sys/class/accel: the devices
/// found, their kinds and names, utilization from two samples of Intel's busy-time counter (none from
/// the first, none where the driver has no counter), a counter that goes backwards, and a rescan that
/// finds a device added.

#include "Platform/GPUTypes.h"
#include "Platform/Linux/AccelNpuProbe.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace Platform
{
namespace
{

using TestSupport::ScopedTempDir;

void writeFile(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << text;
}

/// accel<index> with its device directory at pci/<slot>, bound to @p driver, with @p busyUs if given.
void addDevice(const std::filesystem::path& root,
               int index,
               const std::string& slot,
               const std::string& driver,
               const std::string& vendor,
               std::optional<std::uint64_t> busyUs)
{
    const std::filesystem::path pci = root / "pci" / slot;
    std::filesystem::create_directories(pci);
    std::filesystem::create_directories(root / "drivers" / driver);
    std::filesystem::create_symlink(root / "drivers" / driver, pci / "driver");
    writeFile(pci / "vendor", vendor + "\n");
    if (busyUs.has_value())
    {
        writeFile(pci / "npu_busy_time_us", std::to_string(*busyUs) + "\n");
    }
    const std::filesystem::path accel = root / "class" / ("accel" + std::to_string(index));
    std::filesystem::create_directories(accel);
    std::filesystem::create_directory_symlink(pci, accel / "device");
}

TEST(AccelNpuTest, BusyPercentIsTheBusyShareOfTheWallTime)
{
    EXPECT_DOUBLE_EQ(AccelNpu::busyPercent(1'000, 251'000, 1'000'000'000).value_or(-1.0), 25.0);
    EXPECT_DOUBLE_EQ(AccelNpu::busyPercent(0, 2'000'000, 1'000'000'000).value_or(-1.0), 100.0) << "held to 100";
    EXPECT_FALSE(AccelNpu::busyPercent(5, 4, 1'000'000'000).has_value()) << "a reset counter";
    EXPECT_FALSE(AccelNpu::busyPercent(1, 2, 0).has_value()) << "no time between samples";
}

TEST(AccelNpuTest, NamesAndKindsFollowTheDriver)
{
    EXPECT_EQ(AccelNpu::displayName("intel_vpu"), "Intel NPU");
    EXPECT_EQ(AccelNpu::displayName("amdxdna"), "AMD NPU");
    EXPECT_EQ(AccelNpu::displayName("habanalabs"), "Accelerator (habanalabs)");
    EXPECT_EQ(AccelNpu::kindOf("intel_vpu"), DeviceKind::Npu);
    EXPECT_EQ(AccelNpu::kindOf("amdxdna"), DeviceKind::Npu);
    EXPECT_EQ(AccelNpu::kindOf("habanalabs"), DeviceKind::ComputeAccelerator);
}

TEST(AccelNpuProbeTest, ListsTheDevicesAndSamplesIntelsBusyTime)
{
    const ScopedTempDir dir("tasksmack_accel_npu");
    addDevice(dir.path, 0, "0000:00:0b.0", "intel_vpu", "0x8086", 1'000);
    addDevice(dir.path, 1, "0000:c5:00.1", "amdxdna", "0x1022", std::nullopt);
    std::uint64_t now = 5'000'000'000;
    AccelNpuProbe probe(dir.path / "class", [&now] { return now; });
    ASSERT_TRUE(probe.isAvailable());

    const std::vector<GPUInfo> devices = probe.enumerateGPUs();
    ASSERT_EQ(devices.size(), 2U);
    EXPECT_EQ(devices[0].id, "ACCEL_0000:00:0b.0");
    EXPECT_EQ(devices[0].name, "Intel NPU");
    EXPECT_EQ(devices[0].vendor, "Intel");
    EXPECT_EQ(devices[0].kind, DeviceKind::Npu);
    EXPECT_TRUE(devices[0].isIntegrated);
    EXPECT_EQ(devices[1].name, "AMD NPU");
    EXPECT_EQ(devices[1].vendor, "AMD");
    EXPECT_EQ(devices[1].deviceIndex, 1U);

    // The first sample only sets the baseline.
    std::vector<GPUCounters> counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[0].gpuId, devices[0].id);
    EXPECT_EQ(counters[0].kind, DeviceKind::Npu);
    EXPECT_FALSE(counters[0].utilizationAvailable);
    EXPECT_FALSE(counters[0].temperatureAvailable);
    EXPECT_FALSE(counters[0].memoryAvailable);

    // A second, a second later, 400 ms busier: 40 %. AMD has no counter, so no utilization at all.
    writeFile(dir.path / "pci" / "0000:00:0b.0" / "npu_busy_time_us", "401000\n");
    now += 1'000'000'000;
    counters = probe.readGPUCounters();
    EXPECT_TRUE(counters[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 40.0);
    EXPECT_FALSE(counters[1].utilizationAvailable);

    // A counter that went backwards (the device reset) gives no reading, then a new baseline.
    writeFile(dir.path / "pci" / "0000:00:0b.0" / "npu_busy_time_us", "10\n");
    now += 1'000'000'000;
    EXPECT_FALSE(probe.readGPUCounters()[0].utilizationAvailable);

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
    EXPECT_TRUE(probe.capabilities().supportsMultiGPU);
}

TEST(AccelNpuProbeTest, NoneListedAndARescanFindsOneAdded)
{
    const ScopedTempDir dir("tasksmack_accel_npu_rescan");
    std::filesystem::create_directories(dir.path / "class");
    AccelNpuProbe probe(dir.path / "class", [] { return std::uint64_t{1}; });
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    addDevice(dir.path, 0, "0000:00:0b.0", "intel_vpu", "0x8086", 0);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "only a full rescan looks";
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "nothing changed since";

    // A missing class directory (no accel subsystem) is simply no devices.
    const AccelNpuProbe none(dir.path / "absent");
    EXPECT_FALSE(none.isAvailable());
}

} // namespace
} // namespace Platform
