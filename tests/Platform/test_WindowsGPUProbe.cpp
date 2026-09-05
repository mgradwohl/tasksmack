#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/WindowsGPUProbe.h"
#include "Platform/Windows/WindowsGPUProbeMath.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Platform
{
namespace
{

// ==========================================================================
// normalizeGPUName / gpuNamesMatch: pure string logic, no hardware required.
// ==========================================================================

TEST(NormalizeGPUNameTest, LowercasesAndTrims)
{
    EXPECT_EQ(normalizeGPUName("  NVIDIA GeForce RTX 4090  "), "nvidia geforce rtx 4090");
}

TEST(NormalizeGPUNameTest, CollapsesInternalWhitespace)
{
    EXPECT_EQ(normalizeGPUName("NVIDIA   GeForce\tRTX  4090"), "nvidia geforce rtx 4090");
}

TEST(NormalizeGPUNameTest, EmptyStringStaysEmpty)
{
    EXPECT_EQ(normalizeGPUName(""), "");
    EXPECT_EQ(normalizeGPUName("   "), "");
}

TEST(GpuNamesMatchTest, ExactMatch)
{
    EXPECT_TRUE(gpuNamesMatch("GeForce RTX 4090", "GeForce RTX 4090"));
}

TEST(GpuNamesMatchTest, CaseAndWhitespaceInsensitiveMatch)
{
    EXPECT_TRUE(gpuNamesMatch("NVIDIA GeForce RTX 4090", "nvidia   geforce rtx 4090"));
}

TEST(GpuNamesMatchTest, SubstringMatch)
{
    // DXGI often reports "NVIDIA GeForce RTX 4090" while NVML reports just "GeForce RTX 4090".
    EXPECT_TRUE(gpuNamesMatch("NVIDIA GeForce RTX 4090", "GeForce RTX 4090"));
    EXPECT_TRUE(gpuNamesMatch("GeForce RTX 4090", "NVIDIA GeForce RTX 4090"));
}

TEST(GpuNamesMatchTest, UnrelatedNamesDoNotMatch)
{
    EXPECT_FALSE(gpuNamesMatch("NVIDIA GeForce RTX 4090", "AMD Radeon RX 7900"));
}

TEST(GpuNamesMatchTest, EmptyNameNeverMatchesANonEmptyName)
{
    // NVML leaves a GPU's name empty when DeviceGetName fails for that device; such a device
    // must never be spuriously matched to a real, named DXGI adapter via the substring check.
    EXPECT_FALSE(gpuNamesMatch("NVIDIA GeForce RTX 4090", ""));
    EXPECT_FALSE(gpuNamesMatch("", "NVIDIA GeForce RTX 4090"));
}

TEST(GpuNamesMatchTest, TwoEmptyNamesDoNotMatch)
{
    EXPECT_FALSE(gpuNamesMatch("", ""));
}

TEST(GpuNamesMatchTest, AllWhitespaceNameNeverMatchesANonEmptyName)
{
    EXPECT_FALSE(gpuNamesMatch("NVIDIA GeForce RTX 4090", "   "));
}

TEST(GpuNamesMatchTest, TwoDistinctAllWhitespaceNamesDoNotMatch)
{
    // A raw exact-match shortcut (name1 == name2) taken before normalization would otherwise
    // treat two byte-identical all-whitespace names as a match.
    EXPECT_FALSE(gpuNamesMatch("   ", "   "));
}

// ==========================================================================
// mergeNVMLIntoDXGICounters / allGPUsHaveNVMLUtilization / sumProcessUtilizationByGPUId /
// assignPDHUtilizationToDXGICounters: pure merge logic extracted from WindowsGPUProbe, no
// NVML/PDH hardware required.
// ==========================================================================

TEST(MergeNVMLIntoDXGICountersTest, EmptyNVMLCountersLeavesDXGICountersUntouchedAndReturnsEmptySet)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 42.0;

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, {}, {{0, 0}});

    EXPECT_TRUE(sourced.empty());
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 42.0);
}

TEST(MergeNVMLIntoDXGICountersTest, UnmappedDXGIIndexIsSkipped)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 5.0;

    std::vector<GPUCounters> nvml(1);
    nvml[0].utilizationPercent = 99.0;

    // No mapping for DXGI index 0 -> nothing should change.
    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {});

    EXPECT_TRUE(sourced.empty());
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 5.0);
}

TEST(MergeNVMLIntoDXGICountersTest, OutOfRangeNVMLIndexIsSkipped)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";

    const std::vector<GPUCounters> nvml(1); // only index 0 valid

    // Mapping points at NVML index 5, which doesn't exist.
    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 5}});

    EXPECT_TRUE(sourced.empty());
}

TEST(MergeNVMLIntoDXGICountersTest, MergesMappedGPUAndReportsIdAsSourced)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 0.0;
    dxgi[0].memoryUsedBytes = 111;
    dxgi[0].memoryTotalBytes = 0; // DXGI didn't report total memory

    std::vector<GPUCounters> nvml(1);
    nvml[0].temperatureC = 65;
    nvml[0].powerDrawWatts = 150.5;
    nvml[0].powerLimitWatts = 300.0;
    nvml[0].gpuClockMHz = 1800;
    nvml[0].memoryClockMHz = 9500;
    nvml[0].fanSpeedRaw = 40;
    nvml[0].fanSpeedMaxRaw = 100;
    nvml[0].utilizationPercent = 0.0; // idle is a valid NVML reading, must still be applied
    nvml[0].memoryUsedBytes = 222;
    nvml[0].memoryTotalBytes = 8ULL * 1024 * 1024 * 1024;

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}});

    EXPECT_TRUE(sourced.contains("GPU0"));
    EXPECT_EQ(dxgi[0].temperatureC, 65);
    EXPECT_DOUBLE_EQ(dxgi[0].powerDrawWatts, 150.5);
    EXPECT_DOUBLE_EQ(dxgi[0].powerLimitWatts, 300.0);
    EXPECT_EQ(dxgi[0].gpuClockMHz, 1800U);
    EXPECT_EQ(dxgi[0].memoryClockMHz, 9500U);
    EXPECT_EQ(dxgi[0].fanSpeedRaw, 40U);
    EXPECT_EQ(dxgi[0].fanSpeedMaxRaw, 100U);
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 0.0) << "NVML's idle 0% must still overwrite DXGI's stale value";
    // NVML reported a real total, so its memory metrics win over DXGI's.
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 222U);
    EXPECT_EQ(dxgi[0].memoryTotalBytes, 8ULL * 1024 * 1024 * 1024);
}

TEST(MergeNVMLIntoDXGICountersTest, ZeroNVMLMemoryTotalKeepsDXGIMemoryValues)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryUsedBytes = 111;
    dxgi[0].memoryTotalBytes = 999;

    std::vector<GPUCounters> nvml(1);
    nvml[0].memoryUsedBytes = 222;
    nvml[0].memoryTotalBytes = 0; // NVML query failed for memory; DXGI's numbers must survive

    [[maybe_unused]] const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}});

    EXPECT_EQ(dxgi[0].memoryUsedBytes, 111U);
    EXPECT_EQ(dxgi[0].memoryTotalBytes, 999U);
}

TEST(AllGPUsHaveNVMLUtilizationTest, EmptyDXGICountersIsFalse)
{
    EXPECT_FALSE(allGPUsHaveNVMLUtilization({}, {"GPU0"}));
}

TEST(AllGPUsHaveNVMLUtilizationTest, TrueWhenEveryGPUIsSourced)
{
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[1].gpuId = "GPU1";

    EXPECT_TRUE(allGPUsHaveNVMLUtilization(dxgi, {"GPU0", "GPU1"}));
}

TEST(AllGPUsHaveNVMLUtilizationTest, FalseWhenAnyGPUIsMissing)
{
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[1].gpuId = "GPU1";

    EXPECT_FALSE(allGPUsHaveNVMLUtilization(dxgi, {"GPU0"}));
}

TEST(SumProcessUtilizationByGPUIdTest, SumsMultipleProcessesOnSameGPU)
{
    std::vector<ProcessGPUCounters> procs(2);
    procs[0].gpuId = "GPU_0x0_0x1";
    procs[0].gpuUtilPercent = 30.0;
    procs[1].gpuId = "GPU_0x0_0x1";
    procs[1].gpuUtilPercent = 45.0;

    const auto byGpu = sumProcessUtilizationByGPUId(procs);

    ASSERT_TRUE(byGpu.contains("GPU_0x0_0x1"));
    EXPECT_DOUBLE_EQ(byGpu.at("GPU_0x0_0x1"), 75.0);
}

TEST(SumProcessUtilizationByGPUIdTest, SkipsNonPositiveUtilization)
{
    std::vector<ProcessGPUCounters> procs(2);
    procs[0].gpuId = "GPU_0x0_0x1";
    procs[0].gpuUtilPercent = 0.0;
    procs[1].gpuId = "GPU_0x0_0x1";
    procs[1].gpuUtilPercent = -1.0; // shouldn't occur in practice, but must not create a bucket

    const auto byGpu = sumProcessUtilizationByGPUId(procs);

    EXPECT_TRUE(byGpu.empty());
}

TEST(AssignPDHUtilizationToDXGICountersTest, AssignsClampedUtilizationForMatchedLuid)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 0.0;

    const std::unordered_map<std::string, double> byLuid = {{"GPU_0xLUID", 150.0}}; // exceeds 100
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID"}};

    assignPDHUtilizationToDXGICounters(dxgi, byLuid, idToLuid, {});

    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 100.0) << "summed per-engine utilization must clamp to 100";
}

TEST(AssignPDHUtilizationToDXGICountersTest, SkipsGPUsAlreadySourcedFromNVML)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 7.0;

    const std::unordered_map<std::string, double> byLuid = {{"GPU_0xLUID", 50.0}};
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID"}};

    assignPDHUtilizationToDXGICounters(dxgi, byLuid, idToLuid, {"GPU0"});

    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 7.0) << "NVML-sourced GPUs must not be overwritten by PDH";
}

TEST(AssignPDHUtilizationToDXGICountersTest, LeavesUtilizationUntouchedWhenNoLuidMapping)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 3.0;

    // No entry for "GPU0" in idToLuid: enumerateGPUs() hasn't populated it yet.
    assignPDHUtilizationToDXGICounters(dxgi, {{"GPU_0xLUID", 50.0}}, {}, {});

    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 3.0);
}

TEST(AssignPDHUtilizationToDXGICountersTest, LeavesUtilizationUntouchedWhenLuidHasNoPDHData)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 3.0;

    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID"}};

    // byLuid has data for a different LUID only.
    assignPDHUtilizationToDXGICounters(dxgi, {{"GPU_0xOther", 50.0}}, idToLuid, {});

    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 3.0);
}

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if underlying probes are unavailable
    EXPECT_NO_THROW(WindowsGPUProbe probe);
}

TEST(WindowsGPUProbeTest, BasicOperationsDoNotThrow)
{
    WindowsGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
}

// ==========================================================================
// Enumeration Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, EnumerateGPUsSmokeReturnsWellFormedDataWhenPresent)
{
    WindowsGPUProbe probe;
    auto gpus = probe.enumerateGPUs();

    std::unordered_set<std::string> ids;

    // If GPUs are present, validate their fields
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty";
        EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty";
        EXPECT_TRUE(ids.insert(gpu.id).second) << "GPU ids should be unique";
        // luidId may be present for DXGI enumeration
    }
}

TEST(WindowsGPUProbeTest, EnumerateGPUsIsDeterministic)
{
    WindowsGPUProbe probe;

    auto gpus1 = probe.enumerateGPUs();
    auto gpus2 = probe.enumerateGPUs();

    // Enumeration should be deterministic (same list on consecutive calls)
    EXPECT_EQ(gpus1.size(), gpus2.size()) << "GPU count should be consistent across calls";

    // IDs should match in order
    for (std::size_t i = 0; i < gpus1.size(); ++i)
    {
        EXPECT_EQ(gpus1[i].id, gpus2[i].id) << "GPU id should be consistent at index " << i;
        EXPECT_EQ(gpus1[i].name, gpus2[i].name) << "GPU name should be consistent at index " << i;
    }
}

// ==========================================================================
// Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;

    // Enumerate first (required for LUID mapping in counter reading)
    auto gpus = probe.enumerateGPUs();

    // Now read counters
    auto counters = probe.readGPUCounters();

    // Counter list should match GPU list size (or be empty if no GPUs)
    if (gpus.empty())
    {
        EXPECT_EQ(counters.size(), 0UL) << "No counters should be returned if no GPUs enumerated";
    }
    else
    {
        EXPECT_EQ(counters.size(), gpus.size()) << "Counter count should match GPU count";

        // Validate counter fields
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty()) << "Counter gpuId should not be empty";
            EXPECT_GE(counter.utilizationPercent, 0.0) << "Utilization should be >= 0";
            EXPECT_LE(counter.utilizationPercent, 100.0) << "Utilization should be <= 100";
        }
    }
}

TEST(WindowsGPUProbeTest, ReadGPUCountersAfterEnumerateIsConsistent)
{
    WindowsGPUProbe probe;

    auto gpus = probe.enumerateGPUs();
    auto counters1 = probe.readGPUCounters();
    auto counters2 = probe.readGPUCounters();

    // Counter list should be consistent
    EXPECT_EQ(counters1.size(), counters2.size()) << "Counter count should be consistent across calls";

    // GPUs should remain enumerated
    auto gpus2 = probe.enumerateGPUs();
    EXPECT_EQ(gpus.size(), gpus2.size()) << "GPU count should not change";
}

// ==========================================================================
// Process Counter Reading Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ReadProcessGPUCountersReturnsValidList)
{
    WindowsGPUProbe probe;
    auto counters = probe.readProcessGPUCounters();

    // Per-process GPU counters may be empty (no GPU-using processes)
    // or populated if processes are using GPU resources
    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process ID should be non-negative";
        // Other fields may vary depending on availability
    }
}

// ==========================================================================
// State Consistency Tests
// ==========================================================================

TEST(WindowsGPUProbeTest, ProbeStatePersistsBetweenCalls)
{
    WindowsGPUProbe probe;

    // First enumeration
    auto gpus1 = probe.enumerateGPUs();
    auto caps1 = probe.capabilities();

    // Second enumeration
    auto gpus2 = probe.enumerateGPUs();
    auto caps2 = probe.capabilities();

    // State should be consistent
    EXPECT_EQ(gpus1.size(), gpus2.size());
    // Capabilities should match (at least structure-wise)
    EXPECT_EQ(caps1.hasTemperature, caps2.hasTemperature);
    EXPECT_EQ(caps1.hasPowerMetrics, caps2.hasPowerMetrics);
}

TEST(WindowsGPUProbeTest, CapabilitiesReturnsConsistentStructure)
{
    WindowsGPUProbe probe;
    auto caps = probe.capabilities();

    // The public contract only guarantees stable self-consistency and the
    // per-process/engine-utilization relationship.
    EXPECT_EQ(caps.hasPerProcessMetrics, caps.hasEngineUtilization);
}

} // namespace
} // namespace Platform

#endif // _WIN32
