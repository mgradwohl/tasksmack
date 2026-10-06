#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/PDHGPUProbe.h"
#include "Platform/Windows/WindowsGPUProbe.h"
#include "Platform/Windows/WindowsGPUProbeMath.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
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
// mergeNVMLIntoDXGICounters / allGPUsHaveNVMLUtilization /
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

TEST(MergeNVMLIntoDXGICountersTest, FailedNVMLUtilizationReadLeavesThePDHFallbackAvailable)
{
    // #1111: NVML's utilization read failed (timeout, TDR). The GPU must not be marked NVML-sourced
    // with a real-looking 0%, which suppressed the valid PDH utilization; the read validity of the
    // other fields comes along so they publish as gaps. (NVML's utilization is taken only without
    // PDH, #1264.)
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 37.0; // a later PDH merge fills this in

    std::vector<GPUCounters> nvml(1);
    nvml[0].gpuId = "uuid-0";
    nvml[0].utilizationAvailable = false;
    nvml[0].utilizationPercent = 0.0;
    nvml[0].temperatureAvailable = false;
    nvml[0].powerAvailable = true;
    nvml[0].powerDrawWatts = 80.0;

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}}, nullptr, /*takeUtilization=*/true);

    EXPECT_FALSE(sourced.contains("GPU0"));
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 37.0);
    // Unread until PDH supplies a reading, so no PDH sample means a gap, not DXGI's placeholder.
    EXPECT_FALSE(dxgi[0].utilizationAvailable);
    EXPECT_FALSE(dxgi[0].temperatureAvailable);
    EXPECT_TRUE(dxgi[0].powerAvailable);
    EXPECT_DOUBLE_EQ(dxgi[0].powerDrawWatts, 80.0);
}

TEST(MergeNVMLIntoDXGICountersTest, NVMLReadingsMakeDXGIsUnreadFieldsAvailable)
{
    // #1245: DXGI's counters start unread; NVML's successful reads make them available.
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationAvailable = false;
    dxgi[0].memoryAvailable = false;
    std::vector<GPUCounters> nvml(1);
    nvml[0].gpuId = "uuid-0";
    nvml[0].utilizationPercent = 0.0; // A real idle reading
    nvml[0].memoryTotalBytes = 8ULL << 30U;
    nvml[0].memoryUsedBytes = 1ULL << 30U;

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}}, nullptr, /*takeUtilization=*/true);

    EXPECT_TRUE(sourced.contains("GPU0"));
    EXPECT_TRUE(dxgi[0].utilizationAvailable);
    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 1ULL << 30U);
}

TEST(MergeNVMLIntoDXGICountersTest, UnmappedDXGIIndexIsSkipped)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 5.0;

    std::vector<GPUCounters> nvml(1);
    nvml[0].gpuId = "uuid-0";
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
    nvml[0].gpuId = "uuid-0";
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

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}}, nullptr, /*takeUtilization=*/true);

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
    nvml[0].gpuId = "uuid-0";
    nvml[0].memoryUsedBytes = 222;
    nvml[0].memoryTotalBytes = 0; // NVML query failed for memory; DXGI's numbers must survive

    [[maybe_unused]] const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}});

    EXPECT_EQ(dxgi[0].memoryUsedBytes, 111U);
    EXPECT_EQ(dxgi[0].memoryTotalBytes, 999U);
}

// #1264: an NVIDIA adapter's utilization is PDH's, like every other adapter's and every process's,
// not NVML's util.gpu, which disagreed with Task Manager and the per-process sum. The NVML merge
// leaves it alone and claims no GPU, so the PDH merge assigns it.
TEST(MergeNVMLIntoDXGICountersTest, PDHUtilizationWinsForAnNVMLCoveredAdapter)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationAvailable = false; // DXGI's counter starts unread (#1245)
    std::vector<GPUCounters> nvml(1);
    nvml[0].gpuId = "uuid-0";
    nvml[0].utilizationPercent = 97.0; // NVML: "a kernel ran" in its last window
    nvml[0].temperatureC = 70;

    std::unordered_set<std::string> memoryIds;
    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}}, &memoryIds);
    EXPECT_TRUE(sourced.empty());
    EXPECT_EQ(dxgi[0].temperatureC, 70); // Sensors still come from NVML

    const std::unordered_map<std::string, double> byLuid = {{"GPU_0xLUID0", 41.5}};
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID0"}};
    assignPDHUtilizationToDXGICounters(dxgi, byLuid, idToLuid, sourced, /*absentMeansIdle=*/true);

    EXPECT_TRUE(dxgi[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 41.5);
}

// #1265: NVML left a sleeping GPU alone. The merged counter says so ("(Sleeping)" in the header)
// and keeps NVML's last VRAM total, but its memory in use isn't NVML's: PDH's adapter-wide figure,
// read without touching the GPU, fills it in.
TEST(MergeNVMLIntoDXGICountersTest, ASleepingGpuIsSuspendedAndTakesPDHMemory)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryTotalBytes = 7ULL << 30U; // DXGI's dedicated figure
    dxgi[0].memoryAvailable = false;
    std::vector<GPUCounters> nvml(1);
    nvml[0].gpuId = "uuid-0";
    nvml[0].suspended = true;
    nvml[0].utilizationAvailable = false;
    nvml[0].temperatureAvailable = false;
    nvml[0].powerAvailable = false;
    nvml[0].gpuClockAvailable = false;
    nvml[0].memoryAvailable = false;
    nvml[0].memoryTotalBytes = 8ULL << 30U; // Last read while awake

    std::unordered_set<std::string> memoryIds;
    [[maybe_unused]] const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0, 0}}, &memoryIds);

    EXPECT_TRUE(dxgi[0].suspended);
    EXPECT_FALSE(dxgi[0].temperatureAvailable);
    EXPECT_EQ(dxgi[0].memoryTotalBytes, 8ULL << 30U);
    EXPECT_TRUE(memoryIds.empty());

    AdapterMemoryUsage usage{};
    usage.dedicatedBytes = 300ULL << 20U;
    usage.dedicatedRead = true;
    assignPDHMemoryToDXGICounters(dxgi, {{"GPU_0xLUID0", usage}}, {{"GPU0", "GPU_0xLUID0"}}, {{"GPU0", false}}, memoryIds);
    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 300ULL << 20U);
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

TEST(AssignPDHUtilizationToDXGICountersTest, PDHReadingRestoresAvailabilityAfterAFailedNVMLRead)
{
    // #1111: a GPU whose NVML utilization read failed is marked unread by the merge; PDH's reading
    // is real, so it makes the field available again. Without PDH data it stays unread (a gap).
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationAvailable = false;
    dxgi[1].gpuId = "GPU1";
    dxgi[1].utilizationAvailable = false;

    const std::unordered_map<std::string, double> byLuid = {{"GPU_0xLUID0", 42.0}};
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID0"}, {"GPU1", "GPU_0xLUID1"}};

    assignPDHUtilizationToDXGICounters(dxgi, byLuid, idToLuid, {});

    EXPECT_TRUE(dxgi[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 42.0);
    EXPECT_FALSE(dxgi[1].utilizationAvailable);
}

// #1166: PDH has GPU Engine instances only for processes using an adapter, so after a successful
// collect an adapter with none is idle -- 0% -- not a permanent gap. Without a successful collect
// (warm-up, failure) it stays unread.
TEST(AssignPDHUtilizationToDXGICountersTest, AnAdapterWithNoEngineActivityIsIdleAfterASuccessfulCollect)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0xLUID"}};

    assignPDHUtilizationToDXGICounters(dxgi, {}, idToLuid, {}, /*absentMeansIdle=*/true);
    EXPECT_TRUE(dxgi[0].utilizationAvailable);
    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 0.0);

    assignPDHUtilizationToDXGICounters(dxgi, {}, idToLuid, {}, /*absentMeansIdle=*/false);
    EXPECT_FALSE(dxgi[0].utilizationAvailable);

    // #1277 review: an adapter whose engine items were all unreadable is unread, not idle.
    assignPDHUtilizationToDXGICounters(dxgi, {}, idToLuid, {}, /*absentMeansIdle=*/true, {"GPU_0xLUID"});
    EXPECT_FALSE(dxgi[0].utilizationAvailable);
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
    EXPECT_TRUE(dxgi[0].utilizationAvailable);
}

TEST(AssignPDHUtilizationToDXGICountersTest, LeavesUtilizationUntouchedWhenNoLuidMapping)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].utilizationPercent = 3.0;

    // No entry for "GPU0" in idToLuid: enumerateGPUs() hasn't populated it yet.
    assignPDHUtilizationToDXGICounters(dxgi, {{"GPU_0xLUID", 50.0}}, {}, {});

    EXPECT_DOUBLE_EQ(dxgi[0].utilizationPercent, 3.0);
    EXPECT_FALSE(dxgi[0].utilizationAvailable) << "no PDH reading: a gap, not a real-looking value (#1111)";
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
    EXPECT_FALSE(dxgi[0].utilizationAvailable) << "no PDH reading: a gap, not a real-looking value (#1111)";
}

// ==========================================================================
// Basic Smoke Tests
// ==========================================================================

// =============================================================================
// mapDXGIToNVML / assignPDHMemoryToDXGICounters (#1040, #1029)
// =============================================================================

namespace
{
GPUInfo makeInfo(const std::string& name, const std::string& vendor)
{
    GPUInfo info;
    info.name = name;
    info.vendor = vendor;
    return info;
}
} // namespace

GPUInfo makeLocatedInfo(const std::string& name, std::uint32_t bus, std::uint32_t pciDeviceId = 0)
{
    GPUInfo info = makeInfo(name, "NVIDIA");
    info.pciLocation = PciLocation{.bus = bus, .device = 0, .function = std::nullopt};
    info.pciDeviceId = pciDeviceId;
    return info;
}

// #1091: two identical cards, with the monitor on the one at bus 0x02, so DXGI lists it first while
// NVML orders by bus. The PCI location pairs each adapter with its own device.
TEST(MapDXGIToNVMLTest, IdenticalCardsMapByPciLocationWhateverTheOrder)
{
    const std::vector<GPUInfo> dxgi = {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02), makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01)};
    const std::vector<GPUInfo> nvml = {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01), makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02)};

    const auto mapping = mapDXGIToNVML(dxgi, nvml);
    ASSERT_EQ(mapping.size(), 2U);
    EXPECT_EQ(mapping.at(0), 1U);
    EXPECT_EQ(mapping.at(1), 0U);
}

// #1317: each mapped NVML device is named by its DXGI adapter's id, found through the mapping, not
// by its own position in NVML's order; an unmapped device has no entry.
TEST(NVMLDeviceAdapterIdsTest, EachMappedDeviceTakesItsAdaptersId)
{
    std::vector<GPUInfo> dxgi = {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02), makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01)};
    dxgi[0].id = "PCI_02:00.0_10DE:2684";
    dxgi[1].id = "PCI_01:00.0_10DE:2684";
    std::vector<GPUInfo> nvml = {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01),
                                 makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02),
                                 makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x03)};
    nvml[0].id = "GPU-aaaa";
    nvml[1].id = "GPU-bbbb";
    nvml[2].id = "GPU-cccc";

    const auto ids = nvmlDeviceAdapterIds(dxgi, nvml, mapDXGIToNVML(dxgi, nvml));
    ASSERT_EQ(ids.size(), 2U);
    EXPECT_EQ(ids.at("GPU-aaaa"), "PCI_01:00.0_10DE:2684");
    EXPECT_EQ(ids.at("GPU-bbbb"), "PCI_02:00.0_10DE:2684");
    EXPECT_FALSE(ids.contains("GPU-cccc"));
}

// #1091: without a location, identical cards cannot be told apart. They stay unmapped rather than
// being paired by enumeration order, which showed one card's sensors as the other's.
TEST(MapDXGIToNVMLTest, IdenticalCardsWithoutLocationStayUnmapped)
{
    const std::vector<GPUInfo> dxgi = {makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA"), makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA")};
    const std::vector<GPUInfo> nvml = {makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA"), makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA")};

    EXPECT_TRUE(mapDXGIToNVML(dxgi, nvml).empty());
}

// #1091: "RTX 4060" is a substring of "RTX 4060 Ti". Enumerated in opposite orders, the 4060 used to
// claim the Ti's NVML device and the two cards' sensors were swapped. Exact names win now.
TEST(MapDXGIToNVMLTest, ExactNameWinsOverASubstringClaim)
{
    const std::vector<GPUInfo> dxgi = {makeInfo("GeForce RTX 4060", "NVIDIA"), makeInfo("GeForce RTX 4060 Ti", "NVIDIA")};
    const std::vector<GPUInfo> nvml = {makeInfo("GeForce RTX 4060 Ti", "NVIDIA"), makeInfo("GeForce RTX 4060", "NVIDIA")};

    const auto mapping = mapDXGIToNVML(dxgi, nvml);
    ASSERT_EQ(mapping.size(), 2U);
    EXPECT_EQ(mapping.at(0), 1U);
    EXPECT_EQ(mapping.at(1), 0U);
}

// DXGI names carry "NVIDIA " and NVML's usually do not; that difference alone still counts as exact.
TEST(MapDXGIToNVMLTest, VendorPrefixDoesNotStopAnExactMatch)
{
    const std::vector<GPUInfo> dxgi = {makeInfo("NVIDIA GeForce RTX 4060 Ti", "NVIDIA"), makeInfo("NVIDIA GeForce RTX 4060", "NVIDIA")};
    const std::vector<GPUInfo> nvml = {makeInfo("GeForce RTX 4060", "NVIDIA"), makeInfo("GeForce RTX 4060 Ti", "NVIDIA")};

    const auto mapping = mapDXGIToNVML(dxgi, nvml);
    ASSERT_EQ(mapping.size(), 2U);
    EXPECT_EQ(mapping.at(0), 1U);
    EXPECT_EQ(mapping.at(1), 0U);
}

// Different PCI device ids are different cards, even when the names would match.
TEST(MapDXGIToNVMLTest, DifferentPciDeviceIdsNeverMatch)
{
    GPUInfo adapter = makeInfo("NVIDIA GeForce RTX 4060", "NVIDIA");
    adapter.pciDeviceId = 0x288210DEU;
    GPUInfo device = makeInfo("GeForce RTX 4060", "NVIDIA");
    device.pciDeviceId = 0x280310DEU;

    EXPECT_TRUE(mapDXGIToNVML({adapter}, {device}).empty());
}

// #1091 review: each probe skips a device it fails to read, so a lone "RTX 4060" adapter and a lone
// "RTX 4060 Ti" device can be all that is left. A name inside another, with NVML's PCI identity
// unknown, does not make them the same card.
TEST(MapDXGIToNVMLTest, SubstringMatchNeedsAKnownEqualPciDeviceId)
{
    GPUInfo adapter = makeInfo("NVIDIA GeForce RTX 4060", "NVIDIA");
    adapter.pciDeviceId = 0x288210DEU;
    const GPUInfo unknownDevice = makeInfo("NVIDIA GeForce RTX 4060 Ti", "NVIDIA");
    EXPECT_TRUE(mapDXGIToNVML({adapter}, {unknownDevice}).empty());
    EXPECT_TRUE(mapDXGIToNVML({makeInfo("NVIDIA GeForce RTX 4060", "NVIDIA")}, {unknownDevice}).empty());

    // The same model on both sides, named differently, still maps by substring.
    GPUInfo laptopAdapter = makeInfo("NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA");
    laptopAdapter.pciDeviceId = 0x28E010DEU;
    GPUInfo knownDevice = makeInfo("GeForce RTX 4060 Laptop", "NVIDIA");
    knownDevice.pciDeviceId = 0x28E010DEU;
    const auto mapping = mapDXGIToNVML({laptopAdapter}, {knownDevice});
    ASSERT_EQ(mapping.size(), 1U);
    EXPECT_EQ(mapping.at(0), 0U);
}

// Two functions of one multi-function device at the same bus and device: the function number pairs
// each adapter with its own NVML device. Where NVML's busId couldn't be read the function is unknown,
// and a device still matches the one adapter at its bus and device.
TEST(MapDXGIToNVMLTest, TheFunctionNumberPairsFunctionsAtOneBusAndDevice)
{
    const auto atFunction = [](std::optional<std::uint32_t> function)
    {
        GPUInfo info = makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01);
        info.pciLocation->function = function;
        return info;
    };
    const std::vector<GPUInfo> dxgi = {atFunction(1), atFunction(0)};
    const std::vector<GPUInfo> nvml = {atFunction(0), atFunction(1)};
    const auto mapping = mapDXGIToNVML(dxgi, nvml);
    ASSERT_EQ(mapping.size(), 2U);
    EXPECT_EQ(mapping.at(0), 1U);
    EXPECT_EQ(mapping.at(1), 0U);

    const auto unknownFunction = mapDXGIToNVML({atFunction(0)}, {atFunction(std::nullopt)});
    ASSERT_EQ(unknownFunction.size(), 1U);
    EXPECT_EQ(unknownFunction.at(0), 0U);

    EXPECT_TRUE(mapDXGIToNVML({atFunction(1)}, {atFunction(0)}).empty()) << "Different functions are different devices";
}

// An exact bus/device/function match is made before any unknown-function one, so the unknown device
// can't make it ambiguous: function 1 takes its own device, and function 0 then takes the one left.
TEST(MapDXGIToNVMLTest, AnExactFunctionMatchIsMadeBeforeAnUnknownFunctionOne)
{
    const auto atFunction = [](std::optional<std::uint32_t> function)
    {
        GPUInfo info = makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01);
        info.pciLocation->function = function;
        return info;
    };
    const std::vector<GPUInfo> dxgi = {atFunction(0), atFunction(1)};
    const std::vector<GPUInfo> nvml = {atFunction(1), atFunction(std::nullopt)};
    const auto mapping = mapDXGIToNVML(dxgi, nvml);
    ASSERT_EQ(mapping.size(), 2U);
    EXPECT_EQ(mapping.at(1), 0U);
    EXPECT_EQ(mapping.at(0), 1U);
}

// Different PCI locations are different cards, even when the names match exactly.
TEST(MapDXGIToNVMLTest, DifferentPciLocationsNeverMatchByName)
{
    EXPECT_TRUE(
        mapDXGIToNVML({makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01)}, {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02)}).empty());
}

TEST(MapDXGIToNVMLTest, NonNVIDIAAdaptersAndSurplusCardsStayUnmapped)
{
    // A hybrid laptop's Intel iGPU is never mapped. Two identical NVIDIA adapters and one NVML device
    // without locations are ambiguous -- either could be it -- so neither is mapped.
    const std::vector<GPUInfo> dxgi = {
        makeInfo("Intel(R) Arc(TM) 140T GPU", "Intel"),
        makeInfo("NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA"),
        makeInfo("NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA"),
    };
    const std::vector<GPUInfo> nvml = {makeInfo("NVIDIA GeForce RTX 4060 Laptop GPU", "NVIDIA")};
    EXPECT_TRUE(mapDXGIToNVML(dxgi, nvml).empty());

    // With locations, the adapter at the device's location takes it and the other stays unmapped.
    const std::vector<GPUInfo> locatedDxgi = {
        makeInfo("Intel(R) Arc(TM) 140T GPU", "Intel"),
        makeLocatedInfo("NVIDIA GeForce RTX 4060 Laptop GPU", 0x02),
        makeLocatedInfo("NVIDIA GeForce RTX 4060 Laptop GPU", 0x01),
    };
    const std::vector<GPUInfo> locatedNvml = {makeLocatedInfo("NVIDIA GeForce RTX 4060 Laptop GPU", 0x01)};
    const auto mapping = mapDXGIToNVML(locatedDxgi, locatedNvml);
    ASSERT_EQ(mapping.size(), 1U);
    EXPECT_EQ(mapping.at(2), 0U);
}

TEST(AssignSensorCapabilitiesTest, EachAdapterTakesItsOwnNVMLDevicesSensors)
{
    // Sensors are per adapter (#1040): the iGPU has none, and of two identical NVIDIA cards the
    // passively cooled one reports no fan. The identical cards are told apart by PCI location (#1091).
    std::vector<GPUInfo> dxgi = {
        makeInfo("Intel(R) Arc(TM) 140T GPU", "Intel"),
        makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01),
        makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02),
    };
    std::vector<GPUInfo> nvml = {makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x01), makeLocatedInfo("NVIDIA GeForce RTX 4090", 0x02)};
    GPUCapabilities cooled;
    cooled.hasTemperature = true;
    cooled.hasFanSpeed = true;
    GPUCapabilities fanless;
    fanless.hasTemperature = true;
    nvml[0].sensorCapabilities = cooled;
    nvml[1].sensorCapabilities = fanless;

    assignSensorCapabilities(dxgi, nvml, mapDXGIToNVML(dxgi, nvml), GPUCapabilities{});

    ASSERT_TRUE(dxgi[0].sensorCapabilities.has_value());
    EXPECT_FALSE(dxgi[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    ASSERT_TRUE(dxgi[1].sensorCapabilities.has_value());
    EXPECT_TRUE(dxgi[1].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    ASSERT_TRUE(dxgi[2].sensorCapabilities.has_value());
    EXPECT_TRUE(dxgi[2].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    EXPECT_FALSE(dxgi[2].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
}

TEST(AssignSensorCapabilitiesTest, AMappedDeviceWithoutItsOwnSetTakesTheNVMLProbes)
{
    std::vector<GPUInfo> dxgi = {makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA")};
    const std::vector<GPUInfo> nvml = {makeInfo("NVIDIA GeForce RTX 4090", "NVIDIA")};
    GPUCapabilities probeCaps;
    probeCaps.hasPowerMetrics = true;

    assignSensorCapabilities(dxgi, nvml, mapDXGIToNVML(dxgi, nvml), probeCaps);

    ASSERT_TRUE(dxgi[0].sensorCapabilities.has_value());
    EXPECT_TRUE(dxgi[0].sensorCapabilities.value_or(GPUCapabilities{}).hasPowerMetrics);
}

TEST(OrderNVMLCountersByIdsTest, CountersFollowEnumerationOrderByDeviceId)
{
    // NVML reads counters from an unordered map, so they can come back in either order; the
    // DXGI-to-NVML mapping holds enumeration positions, so they are put back by id (#1040).
    std::vector<GPUCounters> read(2);
    read[0].gpuId = "GPU-uuid-B";
    read[0].temperatureC = 70;
    read[1].gpuId = "GPU-uuid-A";
    read[1].temperatureC = 40;

    const auto ordered = orderNVMLCountersByIds(read, {"GPU-uuid-A", "GPU-uuid-B", "GPU-uuid-C"});

    ASSERT_EQ(ordered.size(), 3U);
    EXPECT_EQ(ordered[0].gpuId, "GPU-uuid-A");
    EXPECT_EQ(ordered[0].temperatureC, 40);
    EXPECT_EQ(ordered[1].gpuId, "GPU-uuid-B");
    EXPECT_EQ(ordered[1].temperatureC, 70);
    EXPECT_TRUE(ordered[2].gpuId.empty()); // Not read this time: a placeholder the merge skips
}

TEST(MergeNVMLIntoDXGICountersTest, PlaceholderForAnUnreadDeviceIsSkipped)
{
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].temperatureC = 55;
    const std::vector<GPUCounters> nvml(1); // Placeholder: empty gpuId

    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0U, 0U}});

    EXPECT_TRUE(sourced.empty());
    EXPECT_EQ(dxgi[0].temperatureC, 55);
}

TEST(MergeNVMLIntoDXGICountersTest, MemoryIdsListOnlyGPUsWhoseNVMLMemoryReadSucceeded)
{
    // A GPU NVML covers for utilization but whose memory read failed (total 0) must still get the
    // PDH memory fallback, so it is not reported as having NVML memory (#1029).
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[1].gpuId = "GPU1";
    std::vector<GPUCounters> nvml(2);
    nvml[0].gpuId = "uuid-0";
    nvml[0].memoryTotalBytes = 8ULL << 30U;
    nvml[0].memoryUsedBytes = 1ULL << 30U;
    nvml[1].gpuId = "uuid-1"; // Memory read failed
    nvml[1].memoryAvailable = false;

    std::unordered_set<std::string> memoryIds;
    const auto sourced = mergeNVMLIntoDXGICounters(dxgi, nvml, {{0U, 0U}, {1U, 1U}}, &memoryIds, /*takeUtilization=*/true);

    EXPECT_EQ(sourced.size(), 2U);
    EXPECT_EQ(memoryIds, (std::unordered_set<std::string>{"GPU0"}));
    // #1111: unread until PDH supplies the memory, so no PDH reading means a gap, not 0 bytes.
    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_FALSE(dxgi[1].memoryAvailable);
}

TEST(AssignPDHMemoryToDXGICountersTest, AnUnreadSegmentLeavesTheCounterUnavailable)
{
    // #1111: PDH read only the shared segment of a discrete GPU (the dedicated array failed), so its
    // entry exists with dedicatedBytes 0. That is not a reading: the counter stays unread.
    std::vector<GPUCounters> dxgi(1);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryAvailable = false;
    const std::unordered_map<std::string, AdapterMemoryUsage> memory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 0, .sharedBytes = 200, .dedicatedRead = false, .sharedRead = true}},
    };
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0x0_0x1"}};
    const std::unordered_map<std::string, bool> integrated = {{"GPU0", false}};

    assignPDHMemoryToDXGICounters(dxgi, memory, idToLuid, integrated, {});

    EXPECT_FALSE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 0U);
}

TEST(AssignPDHMemoryToDXGICountersTest, ASelectedSegmentReadAsZeroBytesIsAReading)
{
    // #1246: an idle discrete GPU with nothing in dedicated memory reads a real 0 B, not N/A.
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryAvailable = false;
    dxgi[0].memoryUsedBytes = 7;
    dxgi[1].gpuId = "GPU1";
    dxgi[1].memoryAvailable = false;
    const std::unordered_map<std::string, AdapterMemoryUsage> memory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 0, .sharedBytes = 200, .dedicatedRead = true, .sharedRead = true}},
        {"GPU_0x0_0x2", {.dedicatedBytes = 0, .sharedBytes = 0, .dedicatedRead = false, .sharedRead = true}},
    };
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0x0_0x1"}, {"GPU1", "GPU_0x0_0x2"}};
    const std::unordered_map<std::string, bool> integrated = {{"GPU0", false}, {"GPU1", true}};

    assignPDHMemoryToDXGICounters(dxgi, memory, idToLuid, integrated, {});

    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 0U);
    EXPECT_TRUE(dxgi[1].memoryAvailable) << "an integrated GPU's shared segment read as 0 B";
    EXPECT_EQ(dxgi[1].memoryUsedBytes, 0U);
}

TEST(AssignPDHMemoryToDXGICountersTest, PDHReadingRestoresAvailabilityAfterAFailedNVMLRead)
{
    // #1111: a GPU whose NVML memory read failed is marked unread by the merge; PDH's reading is
    // real, so it makes memory available again. Without a PDH reading it stays unread (a gap).
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryAvailable = false;
    dxgi[1].gpuId = "GPU1";
    dxgi[1].memoryAvailable = false;
    const std::unordered_map<std::string, AdapterMemoryUsage> memory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 3'000'000'000, .sharedBytes = 200, .dedicatedRead = true, .sharedRead = true}},
    };
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0x0_0x1"}, {"GPU1", "GPU_0x0_0x2"}};

    assignPDHMemoryToDXGICounters(dxgi, memory, idToLuid, {}, {});

    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 3'000'000'000U);
    EXPECT_FALSE(dxgi[1].memoryAvailable);
}

TEST(AssignPDHMemoryToDXGICountersTest, IntegratedUsesSharedDiscreteUsesDedicated)
{
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[1].gpuId = "GPU1";
    const std::unordered_map<std::string, AdapterMemoryUsage> memory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 128, .sharedBytes = 1'500'000'000, .dedicatedRead = true, .sharedRead = true}},
        {"GPU_0x0_0x2", {.dedicatedBytes = 3'000'000'000, .sharedBytes = 200, .dedicatedRead = true, .sharedRead = true}},
    };
    const std::unordered_map<std::string, std::string> idToLuid = {{"GPU0", "GPU_0x0_0x1"}, {"GPU1", "GPU_0x0_0x2"}};
    const std::unordered_map<std::string, bool> integrated = {{"GPU0", true}, {"GPU1", false}};

    assignPDHMemoryToDXGICounters(dxgi, memory, idToLuid, integrated, {});

    EXPECT_EQ(dxgi[0].memoryUsedBytes, 1'500'000'000ULL);
    EXPECT_EQ(dxgi[1].memoryUsedBytes, 3'000'000'000ULL);
}

TEST(AssignPDHMemoryToDXGICountersTest, LeavesNVMLSourcedAndUnmappedGPUsAlone)
{
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "GPU0";
    dxgi[0].memoryUsedBytes = 42; // From NVML
    dxgi[1].gpuId = "GPU1";       // No LUID mapping
    const std::unordered_map<std::string, AdapterMemoryUsage> memory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 999, .sharedBytes = 0, .dedicatedRead = true, .sharedRead = true}},
    };

    assignPDHMemoryToDXGICounters(dxgi, memory, {{"GPU0", "GPU_0x0_0x1"}}, {{"GPU0", false}}, {"GPU0"});

    EXPECT_EQ(dxgi[0].memoryUsedBytes, 42ULL);
    EXPECT_EQ(dxgi[1].memoryUsedBytes, 0ULL);
    // #1111: NVML's memory stays available; the unmapped GPU has no reading, so it's a gap, not 0 bytes.
    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_FALSE(dxgi[1].memoryAvailable);
}

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

// =============================================================================
// nvmlDevicesToLeaveIdle: NVML queries skipped for an NVIDIA adapter PDH saw idle (#1265)
// =============================================================================

namespace
{
using std::chrono::seconds;
using std::chrono::steady_clock;
} // namespace

// A device PDH saw idle last interval, and that NVML read recently, is left alone; one with any
// engine activity is read.
TEST(NVMLDevicesToLeaveIdleTest, AnAdapterIdleByPDHIsLeftAloneAndABusyOneIsRead)
{
    const std::unordered_map<std::string, std::string> adapters = {{"GPU-idle", "PCI_01:00.0_10DE:2684"},
                                                                   {"GPU-busy", "PCI_41:00.0_10DE:2684"}};
    const std::unordered_map<std::string, double> pdh = {{"PCI_01:00.0_10DE:2684", 0.0}, {"PCI_41:00.0_10DE:2684", 3.0}};
    const steady_clock::time_point start{};
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {{"GPU-idle", start}, {"GPU-busy", start}};

    const auto idle = nvmlDevicesToLeaveIdle(adapters, pdh, lastRead, start + seconds{1});
    EXPECT_EQ(idle, (std::unordered_set<std::string>{"GPU-idle"}));
    EXPECT_EQ(lastRead.at("GPU-idle"), start) << "Not read, so its last read stands";
    EXPECT_EQ(lastRead.at("GPU-busy"), start) << "Recorded by recordNVMLReads() once actually read";
}

// The threshold is "no engine activity": a reading at it counts as activity.
TEST(NVMLDevicesToLeaveIdleTest, AReadingAtTheThresholdIsActivity)
{
    const std::unordered_map<std::string, std::string> adapters = {{"GPU-a", "A"}};
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {{"GPU-a", steady_clock::time_point{}}};
    EXPECT_TRUE(nvmlDevicesToLeaveIdle(adapters, {{"A", NVML_IDLE_PDH_UTILIZATION_PERCENT}}, lastRead, steady_clock::time_point{}).empty());
    EXPECT_FALSE(
        nvmlDevicesToLeaveIdle(adapters, {{"A", NVML_IDLE_PDH_UTILIZATION_PERCENT / 2}}, lastRead, steady_clock::time_point{}).empty());
}

// Without a PDH reading for the adapter (PDH unavailable, warming up, a failed collect), or without
// a matched adapter, the device is read as before; so is one NVML hasn't read yet, so its readings
// exist before they are repeated.
TEST(NVMLDevicesToLeaveIdleTest, NoPDHReadingOrNoEarlierReadMeansRead)
{
    const std::unordered_map<std::string, std::string> adapters = {{"GPU-a", "A"}, {"GPU-b", "B"}};
    const steady_clock::time_point now{};
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {{"GPU-a", now}};
    EXPECT_TRUE(nvmlDevicesToLeaveIdle(adapters, {{"B", 0.0}}, lastRead, now).empty());
    GPUCounters read;
    read.gpuId = "GPU-b";
    recordNVMLReads(lastRead, {read}, {}, now);
    EXPECT_TRUE(lastRead.contains("GPU-b")) << "Read now, so it can be left alone from the next sample";
    EXPECT_EQ(nvmlDevicesToLeaveIdle(adapters, {{"B", 0.0}}, lastRead, now), (std::unordered_set<std::string>{"GPU-b"}));
}

// An adapter that stays idle is still read once NVML's readings reach NVML_IDLE_MAX_READING_AGE, so
// they are never older than that, and then left alone again.
TEST(NVMLDevicesToLeaveIdleTest, IdleReadingsAreRefreshedAtTheMaximumAge)
{
    const std::unordered_map<std::string, std::string> adapters = {{"GPU-a", "A"}};
    const std::unordered_map<std::string, double> pdh = {{"A", 0.0}};
    const steady_clock::time_point start{};
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {{"GPU-a", start}};

    EXPECT_FALSE(nvmlDevicesToLeaveIdle(adapters, pdh, lastRead, start + NVML_IDLE_MAX_READING_AGE - seconds{1}).empty());
    EXPECT_TRUE(nvmlDevicesToLeaveIdle(adapters, pdh, lastRead, start + NVML_IDLE_MAX_READING_AGE).empty());
    GPUCounters read;
    read.gpuId = "GPU-a";
    recordNVMLReads(lastRead, {read}, {}, start + NVML_IDLE_MAX_READING_AGE);
    EXPECT_FALSE(nvmlDevicesToLeaveIdle(adapters, pdh, lastRead, start + NVML_IDLE_MAX_READING_AGE + seconds{1}).empty());
}

// Only a real read restarts the maximum age: a device found asleep (not queried), one whose reads
// all failed, and one left idle keep their last read, so a GPU that slept through its refresh is
// read as soon as it wakes rather than replaying readings from before its sleep (#1338 review).
TEST(NVMLDevicesToLeaveIdleTest, OnlyARealReadIsRecorded)
{
    const steady_clock::time_point start{};
    const steady_clock::time_point now = start + NVML_IDLE_MAX_READING_AGE;
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {
        {"GPU-asleep", start}, {"GPU-failed", start}, {"GPU-idle", start}, {"GPU-read", start}};

    GPUCounters failed;
    failed.gpuId = "GPU-failed";
    failed.utilizationAvailable = false;
    failed.temperatureAvailable = false;
    failed.powerAvailable = false;
    failed.gpuClockAvailable = false;
    failed.memoryAvailable = false;
    GPUCounters asleep = failed;
    asleep.gpuId = "GPU-asleep";
    asleep.suspended = true;
    GPUCounters idle;
    idle.gpuId = "GPU-idle";
    GPUCounters read;
    read.gpuId = "GPU-read";
    read.temperatureAvailable = false; // One failed read among successful ones is still a read

    recordNVMLReads(lastRead, {asleep, failed, idle, read}, {"GPU-idle"}, now);
    EXPECT_EQ(lastRead.at("GPU-asleep"), start);
    EXPECT_EQ(lastRead.at("GPU-failed"), start);
    EXPECT_EQ(lastRead.at("GPU-idle"), start);
    EXPECT_EQ(lastRead.at("GPU-read"), now);
}

// A device no longer matched (removed, or re-enumerated under another id) is forgotten.
TEST(NVMLDevicesToLeaveIdleTest, AnUnmatchedDevicesLastReadIsForgotten)
{
    std::unordered_map<std::string, steady_clock::time_point> lastRead = {{"GPU-gone", steady_clock::time_point{}}};
    EXPECT_TRUE(nvmlDevicesToLeaveIdle({}, {}, lastRead, steady_clock::time_point{}).empty());
    EXPECT_TRUE(lastRead.empty());
}

// An idle NVIDIA adapter's NVML counters are its last reading repeated, still marked available. Its
// memory in use must come from PDH's current collect, not the repeated NVML figure, while the NVML
// total and sensors are kept (#1265).
TEST(ExcludeIdleNVMLMemoryTest, AnIdleAdaptersMemoryInUseIsPDHsCurrentFigure)
{
    std::vector<GPUCounters> dxgi(2);
    dxgi[0].gpuId = "PCI_01:00.0_10DE:2684";
    dxgi[1].gpuId = "PCI_41:00.0_10DE:2684";
    std::vector<GPUCounters> nvml(2);
    for (std::size_t i = 0; i < nvml.size(); ++i)
    {
        nvml[i].gpuId = i == 0 ? "GPU-idle" : "GPU-busy";
        nvml[i].memoryUsedBytes = 1'000'000'000; // The idle one's is a repeated, stale reading
        nvml[i].memoryTotalBytes = 24'000'000'000;
        nvml[i].temperatureC = 40;
    }
    std::unordered_set<std::string> memoryIds;
    static_cast<void>(mergeNVMLIntoDXGICounters(dxgi, nvml, {{0U, 0U}, {1U, 1U}}, &memoryIds));
    ASSERT_EQ(memoryIds.size(), 2U);

    const std::unordered_map<std::string, std::string> adapters = {{"GPU-idle", "PCI_01:00.0_10DE:2684"},
                                                                   {"GPU-busy", "PCI_41:00.0_10DE:2684"}};
    excludeIdleNVMLMemory(memoryIds, {"GPU-idle"}, adapters);
    EXPECT_EQ(memoryIds, (std::unordered_set<std::string>{"PCI_41:00.0_10DE:2684"}));

    const std::unordered_map<std::string, AdapterMemoryUsage> pdhMemory = {
        {"GPU_0x0_0x1", {.dedicatedBytes = 300'000'000, .sharedBytes = 0, .dedicatedRead = true, .sharedRead = true}},
        {"GPU_0x0_0x2", {.dedicatedBytes = 5'000'000'000, .sharedBytes = 0, .dedicatedRead = true, .sharedRead = true}},
    };
    const std::unordered_map<std::string, std::string> idToLuid = {{"PCI_01:00.0_10DE:2684", "GPU_0x0_0x1"},
                                                                   {"PCI_41:00.0_10DE:2684", "GPU_0x0_0x2"}};
    assignPDHMemoryToDXGICounters(dxgi, pdhMemory, idToLuid, {}, memoryIds);

    EXPECT_TRUE(dxgi[0].memoryAvailable);
    EXPECT_EQ(dxgi[0].memoryUsedBytes, 300'000'000U) << "PDH's current figure, not the repeated NVML one";
    EXPECT_EQ(dxgi[0].memoryTotalBytes, 24'000'000'000U) << "NVML's total is kept";
    EXPECT_EQ(dxgi[0].temperatureC, 40) << "And its sensors";
    EXPECT_EQ(dxgi[1].memoryUsedBytes, 1'000'000'000U) << "A device read this sample keeps NVML's memory";
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
