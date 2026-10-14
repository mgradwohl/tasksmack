/// @file test_AccelNpuProbe.cpp
/// @brief NPUs and compute accelerators on Linux (#1827), from a fixture /sys/class/accel: the devices
/// found, their kinds and names, utilization from two samples of Intel's busy-time counter (none from
/// the first, none where the driver has no counter), a counter that goes backwards, and a rescan that
/// finds a device added.

#include "Platform/GPUTypes.h"
#include "Platform/Linux/AccelNpuProbe.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
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
    AccelNpuProbe probe(dir.path / "class", [&now] { return now; }, dir.path / "proc"); // No clients there
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

// #1827: an accel client's fdinfo, in the kernel's drm-usage-stats format.
TEST(AccelNpuTest, FdinfoGivesTheClientIdAndItsResidentMemory)
{
    const AccelNpu::ClientMemory ivpu = AccelNpu::parseFdinfo("pos:\t0\nflags:\t02100002\nmnt_id:\t26\nino:\t1061\n"
                                                              "drm-driver:\tintel_vpu\ndrm-client-id:\t42\n"
                                                              "drm-total-memory:\t65536 KiB\ndrm-shared-memory:\t0\n"
                                                              "drm-active-memory:\t0\ndrm-resident-memory:\t32 MiB\n");
    EXPECT_EQ(ivpu.clientId, std::optional<std::uint64_t>{42});
    EXPECT_EQ(ivpu.bytes, std::optional<std::uint64_t>{32ULL << 20U}) << "resident, not total";

    // Without a resident figure, the total; a bare number is bytes, GiB too.
    EXPECT_EQ(AccelNpu::parseFdinfo("drm-client-id: 7\ndrm-total-memory: 4096\n").bytes, std::optional<std::uint64_t>{4096});
    EXPECT_EQ(AccelNpu::parseFdinfo("drm-total-memory: 2 GiB\n").bytes, std::optional<std::uint64_t>{2ULL << 30U});
    // Not a DRM file, another unit, or too large: no client id or no memory.
    EXPECT_FALSE(AccelNpu::parseFdinfo("pos:\t0\nflags:\t02\n").clientId.has_value());
    EXPECT_FALSE(AccelNpu::parseFdinfo("drm-resident-memory: 3 TiB\n").bytes.has_value());
    EXPECT_FALSE(AccelNpu::parseFdinfo("drm-resident-memory: 18446744073709551615 KiB\n").bytes.has_value());
}

/// A process <pid> in the fake proc tree @p proc with fd @p fd open on @p target and @p fdinfo as its fdinfo.
void addFd(const std::filesystem::path& proc, int pid, int fd, const std::string& target, const std::string& fdinfo)
{
    const std::filesystem::path dir = proc / std::to_string(pid);
    std::filesystem::create_directories(dir / "fd");
    std::filesystem::create_symlink(target, dir / "fd" / std::to_string(fd));
    writeFile(dir / "fdinfo" / std::to_string(fd), fdinfo);
}

// #1827: per-process NPU memory, from the fdinfo of the fds open on the accel node, once per client.
TEST(AccelNpuProbeTest, ProcessesHoldingTheNpuOpenGetItsMemoryPerClient)
{
    const ScopedTempDir dir("tasksmack_accel_npu_clients");
    addDevice(dir.path, 0, "0000:00:0b.0", "intel_vpu", "0x8086", std::nullopt);
    const std::filesystem::path proc = dir.path / "proc";
    // 100: one client on two fds (counted once); 200: two clients; 300: a GPU fd, not the NPU's.
    addFd(proc, 100, 3, "/dev/accel/accel0", "drm-client-id:\t1\ndrm-resident-memory:\t8 MiB\n");
    addFd(proc, 100, 4, "/dev/accel/accel0", "drm-client-id:\t1\ndrm-resident-memory:\t8 MiB\n");
    addFd(proc, 200, 5, "/dev/accel/accel0", "drm-client-id:\t2\ndrm-resident-memory:\t1 MiB\n");
    addFd(proc, 200, 6, "/dev/accel/accel0", "drm-client-id:\t3\ndrm-resident-memory:\t2 MiB\n");
    addFd(proc, 300, 7, "/dev/dri/renderD128", "drm-client-id:\t9\ndrm-resident-memory:\t64 MiB\n");
    std::uint64_t now = 1'000'000'000;
    AccelNpuProbe probe(dir.path / "class", [&now] { return now; }, proc);

    const GPUCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasPerProcessNpuMetrics);
    EXPECT_FALSE(caps.hasPerProcessNpuUtilization) << "neither driver gives per-process utilization";
    EXPECT_FALSE(caps.hasPerProcessMetrics) << "nothing for the GPUs";
    EXPECT_TRUE(probe.enumerateGPUs().front().memoryIsShared);

    const auto memoryOf = [](const std::vector<ProcessGPUCounters>& counters, std::int32_t pid) -> std::optional<std::uint64_t>
    {
        const auto it = std::ranges::find(counters, pid, &ProcessGPUCounters::pid);
        return it == counters.end() ? std::nullopt : std::optional<std::uint64_t>(it->gpuSharedMemoryBytes);
    };
    std::vector<ProcessGPUCounters> counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(memoryOf(counters, 100), std::optional<std::uint64_t>{8ULL << 20U});
    EXPECT_EQ(memoryOf(counters, 200), std::optional<std::uint64_t>{3ULL << 20U});
    EXPECT_FALSE(memoryOf(counters, 300).has_value());
    for (const ProcessGPUCounters& c : counters)
    {
        EXPECT_EQ(c.gpuId, "ACCEL_0000:00:0b.0");
        EXPECT_EQ(c.kind, DeviceKind::Npu);
        EXPECT_EQ(c.gpuMemoryBytes, 0U) << "system memory: shared, not dedicated";
    }

    // A client's figure follows its fdinfo each sample; a process that opens the NPU since the walk shows
    // from the next walk, CLIENT_DISCOVERY_INTERVAL_NS later.
    writeFile(proc / "100" / "fdinfo" / "3", "drm-client-id:\t1\ndrm-resident-memory:\t16 MiB\n");
    writeFile(proc / "100" / "fdinfo" / "4", "drm-client-id:\t1\ndrm-resident-memory:\t16 MiB\n");
    addFd(proc, 400, 8, "/dev/accel/accel0", "drm-client-id:\t4\ndrm-resident-memory:\t4 MiB\n");
    now += 1'000'000'000;
    counters = probe.readProcessGPUCounters();
    EXPECT_EQ(memoryOf(counters, 100), std::optional<std::uint64_t>{16ULL << 20U});
    EXPECT_FALSE(memoryOf(counters, 400).has_value()) << "not walked again yet";
    now += AccelNpuProbe::CLIENT_DISCOVERY_INTERVAL_NS;
    counters = probe.readProcessGPUCounters();
    EXPECT_EQ(memoryOf(counters, 400), std::optional<std::uint64_t>{4ULL << 20U});

    // An fd closed since the walk and its number reused for another file isn't the client any more,
    // even before the next walk: 200's fds 5 and 6 now open a GPU node.
    for (const int fd : {5, 6})
    {
        std::filesystem::remove(proc / "200" / "fd" / std::to_string(fd));
        std::filesystem::create_symlink("/dev/dri/renderD128", proc / "200" / "fd" / std::to_string(fd));
    }
    now += 1'000'000'000;
    counters = probe.readProcessGPUCounters();
    EXPECT_FALSE(memoryOf(counters, 200).has_value());
    EXPECT_TRUE(memoryOf(counters, 100).has_value());
}

// A device that comes back under another accelN keeps its id (its PCI slot); its clients are found by
// the new node after the rescan (#1827).
TEST(AccelNpuProbeTest, ADeviceRenumberedOnARescanIsFoundByItsNewNode)
{
    const ScopedTempDir dir("tasksmack_accel_npu_renumber");
    addDevice(dir.path, 0, "0000:00:0b.0", "intel_vpu", "0x8086", std::nullopt);
    const std::filesystem::path proc = dir.path / "proc";
    std::uint64_t now = 1'000'000'000;
    AccelNpuProbe probe(dir.path / "class", [&now] { return now; }, proc);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);

    // Removed and re-added as accel1; its client opens the new node.
    std::filesystem::remove(dir.path / "class" / "accel0" / "device");
    std::filesystem::remove(dir.path / "class" / "accel0");
    std::filesystem::create_directories(dir.path / "class" / "accel1");
    std::filesystem::create_directory_symlink(dir.path / "pci" / "0000:00:0b.0", dir.path / "class" / "accel1" / "device");
    addFd(proc, 100, 3, "/dev/accel/accel1", "drm-client-id:\t1\ndrm-resident-memory:\t8 MiB\n");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)) << "same id, another node: a change";
    const std::vector<ProcessGPUCounters> counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 100);
    EXPECT_EQ(counters[0].gpuId, "ACCEL_0000:00:0b.0");
    EXPECT_EQ(counters[0].gpuSharedMemoryBytes, 8ULL << 20U);
}

// An accelerator with memory of its own (Gaudi's habanalabs): its clients' memory is dedicated (#1827).
TEST(AccelNpuProbeTest, AnAcceleratorsOwnMemoryIsDedicatedNotShared)
{
    const ScopedTempDir dir("tasksmack_accel_dedicated");
    addDevice(dir.path, 0, "0000:4d:00.0", "habanalabs", "0x1da3", std::nullopt);
    const std::filesystem::path proc = dir.path / "proc";
    addFd(proc, 100, 3, "/dev/accel/accel0", "drm-client-id:\t1\ndrm-resident-memory:\t2 GiB\n");
    AccelNpuProbe probe(dir.path / "class", {}, proc);
    const std::vector<GPUInfo> devices = probe.enumerateGPUs();
    ASSERT_EQ(devices.size(), 1U);
    EXPECT_EQ(devices[0].kind, DeviceKind::ComputeAccelerator);
    EXPECT_FALSE(devices[0].memoryIsShared);
    const std::vector<ProcessGPUCounters> counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuMemoryBytes, 2ULL << 30U);
    EXPECT_EQ(counters[0].gpuSharedMemoryBytes, 0U);
}

TEST(AccelNpuTest, FdinfoToleratesCrlfLinesWithoutAColonAndUnreadableMemory)
{
    // CRLF line ends, a line without a colon, and a memory key with no value: the id still reads, the
    // memory doesn't.
    const AccelNpu::ClientMemory memory = AccelNpu::parseFdinfo("garbage\r\ndrm-client-id:\t5\r\ndrm-resident-memory:\t\r\n");
    EXPECT_EQ(memory.clientId, std::optional<std::uint64_t>{5});
    EXPECT_FALSE(memory.bytes.has_value());
    EXPECT_FALSE(AccelNpu::parseFdinfo("drm-total-memory: lots\n").bytes.has_value());
}

// The walks pass over what isn't theirs: a class entry that isn't accelN, a /proc entry that isn't a
// PID, and a client whose fdinfo has gone since the walk. Without a device nothing is read at all.
TEST(AccelNpuProbeTest, TheWalksPassOverWhatIsntTheirs)
{
    const ScopedTempDir dir("tasksmack_accel_npu_walks");
    addDevice(dir.path, 0, "0000:00:0b.0", "intel_vpu", "0x8086", std::nullopt);
    std::filesystem::create_directories(dir.path / "class" / "card0"); // not an accel node
    const std::filesystem::path proc = dir.path / "proc";
    std::filesystem::create_directories(proc / "self");
    addFd(proc, 100, 3, "/dev/accel/accel0", "drm-client-id:\t1\ndrm-resident-memory:\t8 MiB\n");
    addFd(proc, 200, 4, "/dev/accel/accel0", "drm-client-id:\t2\ndrm-resident-memory:\t1 MiB\n");
    std::uint64_t now = 1'000'000'000;
    AccelNpuProbe probe(dir.path / "class", [&now] { return now; }, proc);
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);

    std::vector<ProcessGPUCounters> counters = probe.readProcessGPUCounters();
    EXPECT_EQ(counters.size(), 2U);

    // 200's fdinfo goes (the process is exiting) while its fd still links to the device.
    std::filesystem::remove(proc / "200" / "fdinfo" / "4");
    now += 1'000'000'000;
    counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters.front().pid, 100);

    const ScopedTempDir empty("tasksmack_accel_npu_none");
    std::filesystem::create_directories(empty.path / "class");
    AccelNpuProbe none(empty.path / "class", {}, proc);
    EXPECT_TRUE(none.readProcessGPUCounters().empty());
}

} // namespace
} // namespace Platform
