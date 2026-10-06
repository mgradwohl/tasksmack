/// @file test_DXGIGPUProbeMath.cpp
/// @brief Unit tests for DXGIGPUProbeMath.h's pure adapter naming, listing and classification logic
///
/// DXGIGPUProbeMath.h includes no Windows header, so these tests build and run on every platform,
/// including Linux CI's sanitizer and coverage jobs (#1133). Tests that need D3DKMT fakes, the PnP
/// manager or real adapters stay in test_WindowsDXGIGPUProbe.cpp.

#include "Platform/GPUTypes.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace Platform
{
namespace
{

TEST(ShouldListAdapterTest, SoftwareFlagIsNeverListed)
{
    EXPECT_FALSE(shouldListAdapter(true, std::nullopt));
    EXPECT_FALSE(shouldListAdapter(true, AdapterTypeBits{}));
}

TEST(ShouldListAdapterTest, IndirectDisplayAndSoftwareDevicesAreNotListed)
{
    EXPECT_FALSE(shouldListAdapter(false, AdapterTypeBits{.softwareDevice = false, .indirectDisplayDevice = true}));
    EXPECT_FALSE(shouldListAdapter(false, AdapterTypeBits{.softwareDevice = true, .indirectDisplayDevice = false}));
}

TEST(ShouldListAdapterTest, AHardwareAdapterOrAnUnknownTypeIsListed)
{
    EXPECT_TRUE(shouldListAdapter(false, AdapterTypeBits{}));
    EXPECT_TRUE(shouldListAdapter(false, std::nullopt));
}

TEST(AdapterMemoryTotalBytesTest, IntegratedUsesSharedSystemMemoryDiscreteUsesDedicated)
{
    // A fixed figure from the adapter description, not the moving per-process budget (#1029).
    constexpr std::uint64_t DEDICATED = 128ULL * 1024 * 1024;
    constexpr std::uint64_t SHARED = 16ULL * 1024 * 1024 * 1024;
    EXPECT_EQ(adapterMemoryTotalBytes(true, DEDICATED, SHARED), SHARED);
    EXPECT_EQ(adapterMemoryTotalBytes(false, DEDICATED, SHARED), DEDICATED);
}

TEST(AdapterMemoryIsSharedTest, IntegratedCountsTheSharedSegmentDiscreteDedicated)
{
    // Published as GPUInfo::memoryIsShared so per-process memory counts the adapter's segment, a 0
    // shared reading included (#1164); the size and PDH used bytes follow the same choice.
    static_assert(adapterMemoryIsShared(true));
    static_assert(!adapterMemoryIsShared(false));
    EXPECT_TRUE(adapterMemoryIsShared(true));
    EXPECT_FALSE(adapterMemoryIsShared(false));
}

TEST(MakeDXGIAdapterCountersTest, UtilizationAndMemoryInUseStartUnread)
{
    // DXGI reads neither: with PDH warming up or unavailable, its placeholder 0% and 0 B published
    // as real samples (#1245). NVML or PDH marks them available when it has a reading.
    const auto counter = makeDXGIAdapterCounters("GPU0", 16ULL << 30U);

    EXPECT_EQ(counter.gpuId, "GPU0");
    EXPECT_EQ(counter.memoryTotalBytes, 16ULL << 30U);
    EXPECT_FALSE(counter.utilizationAvailable);
    EXPECT_FALSE(counter.memoryAvailable);
}

// #1317: an adapter's id is its PCI location, with its vendor and device ids, in hex.
TEST(StableAdapterIdTest, AnAdapterWithAPciLocationIsNamedByIt)
{
    EXPECT_EQ(adapterPciId(PciLocation{.bus = 0x01, .device = 0x00, .function = 0}, 0x10DE, 0x2684), "PCI_01:00.0_10DE:2684");
    EXPECT_EQ(stableAdapterId(PciLocation{.bus = 0xC1, .device = 0x1F, .function = 7}, 0x8086, 0x9A49, 0, 0x100, {}),
              "PCI_C1:1F.7_8086:9A49");
    // A location without a function number (D3DKMT always reports one) is named by bus and device.
    EXPECT_EQ(adapterPciId(PciLocation{.bus = 0x01, .device = 0x00, .function = std::nullopt}, 0x10DE, 0x2684), "PCI_01:00_10DE:2684");
}

// Two display functions of one multi-function device share vendor, device ids, bus and device: the
// function number keeps their ids apart, so neither falls back to a LUID id a driver reset changes.
TEST(StableAdapterIdTest, TwoFunctionsAtOneBusAndDeviceGetDistinctPciIds)
{
    const std::string first = stableAdapterId(PciLocation{.bus = 0x01, .device = 0x00, .function = 0}, 0x10DE, 0x2684, 0, 0x200, {});
    const std::unordered_set<std::string> taken = {first};
    const std::string second = stableAdapterId(PciLocation{.bus = 0x01, .device = 0x00, .function = 1}, 0x10DE, 0x2684, 0, 0x300, taken);
    EXPECT_EQ(first, "PCI_01:00.0_10DE:2684");
    EXPECT_EQ(second, "PCI_01:00.1_10DE:2684");
}

// The NVML-to-DXGI match: bus and device must agree, and the function too where both sides know it.
TEST(SamePciLocationTest, AnUnknownFunctionMatchesAnyFunctionAtTheBusAndDevice)
{
    const PciLocation function0{.bus = 0x01, .device = 0x00, .function = 0};
    const PciLocation function1{.bus = 0x01, .device = 0x00, .function = 1};
    const PciLocation unknown{.bus = 0x01, .device = 0x00, .function = std::nullopt};
    EXPECT_TRUE(samePciLocation(function0, function0));
    EXPECT_FALSE(samePciLocation(function0, function1));
    EXPECT_TRUE(samePciLocation(function0, unknown));
    EXPECT_TRUE(samePciLocation(unknown, function1));
    EXPECT_FALSE(samePciLocation(unknown, PciLocation{.bus = 0x02, .device = 0x00, .function = std::nullopt}));
    EXPECT_FALSE(samePciLocation(function0, PciLocation{.bus = 0x01, .device = 0x01, .function = 0}));
}

// #1317: the LUID names an adapter that reports no PCI location (a remote or virtual adapter).
TEST(StableAdapterIdTest, AnAdapterWithoutAPciLocationIsNamedByItsLuid)
{
    EXPECT_EQ(adapterLuidId(0x1, 0xD3A0), "LUID_0x00000001_0x0000D3A0");
    EXPECT_EQ(stableAdapterId(std::nullopt, 0x1414, 0x8C, 0, 0xD3A0, {}), "LUID_0x00000000_0x0000D3A0");
}

// #1317: two adapters never share an id: a second adapter at a location already named gets its LUID.
TEST(StableAdapterIdTest, ASecondAdapterAtATakenLocationIsNamedByItsLuid)
{
    const PciLocation location{.bus = 0x01, .device = 0x00, .function = 0};
    const std::unordered_set<std::string> taken = {adapterPciId(location, 0x10DE, 0x2684)};
    EXPECT_EQ(stableAdapterId(location, 0x10DE, 0x2684, 0, 0x500, taken), "LUID_0x00000000_0x00000500");
    EXPECT_EQ(stableAdapterId(PciLocation{.bus = 0x41, .device = 0x00, .function = 0}, 0x10DE, 0x2684, 0, 0x600, taken),
              "PCI_41:00.0_10DE:2684");
}

// #1317: the id depends on the slot and model, not on where DXGI lists the adapter or the LUID it has
// now. A different model in the slot is a different GPU; an identical one would take the same id.
TEST(StableAdapterIdTest, TheIdIgnoresTheLuidWhenThereIsAPciLocation)
{
    const PciLocation location{.bus = 0x01, .device = 0x00, .function = 0};
    EXPECT_EQ(stableAdapterId(location, 0x10DE, 0x2684, 0, 0x200, {}), stableAdapterId(location, 0x10DE, 0x2684, 0, 0x210, {}));
    EXPECT_NE(stableAdapterId(location, 0x10DE, 0x2684, 0, 0x200, {}), stableAdapterId(location, 0x10DE, 0x2782, 0, 0x200, {}))
        << "A different model later fitted at the same location is a different GPU";
}

TEST(VendorIdToNameTest, KnownVendorIdsMapCorrectly)
{
    EXPECT_EQ(vendorIdToName(0x10DE), "NVIDIA");
    EXPECT_EQ(vendorIdToName(0x1002), "AMD");
    EXPECT_EQ(vendorIdToName(0x1022), "AMD");
    EXPECT_EQ(vendorIdToName(0x8086), "Intel");
    EXPECT_EQ(vendorIdToName(0x8087), "Intel");
    EXPECT_EQ(vendorIdToName(0x5143), "Qualcomm"); // #1263
}

TEST(VendorIdToNameTest, UnknownVendorIdMapsToUnknown)
{
    EXPECT_EQ(vendorIdToName(0x1234), "Unknown");
    EXPECT_EQ(vendorIdToName(0), "Unknown");
}

// =============================================================================
// luidToPdhFormat: pure formatting, no hardware required.
// =============================================================================

TEST(LuidToPdhFormatTest, FormatsHighAndLowPartsAsHex)
{
    EXPECT_EQ(luidToPdhFormat(0, 0xD3A0), "GPU_0x00000000_0x0000D3A0");
    EXPECT_EQ(luidToPdhFormat(0xFFFFFFFF, 1), "GPU_0xFFFFFFFF_0x00000001");
}

// =============================================================================
// isIntegratedGPUFromDesc: pure vendor/VRAM-threshold decision logic, no hardware
// or COM adapter mocking required.
// =============================================================================

TEST(IsIntegratedGPUFromDescTest, SoftwareAdapterIsNeverIntegrated)
{
    constexpr uint32_t SOFTWARE_FLAG = 2;
    // Even an Intel vendor ID with tiny VRAM should report false for a software adapter.
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x8086, SOFTWARE_FLAG, 0));
}

TEST(IsIntegratedGPUFromDescTest, IntelBelowThresholdIsIntegrated)
{
    constexpr uint64_t belowThreshold = (512ULL * 1024 * 1024) - 1;
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x8086, 0, belowThreshold));
}

TEST(IsIntegratedGPUFromDescTest, IntelAtOrAboveThresholdIsDiscrete)
{
    constexpr uint64_t atThreshold = 512ULL * 1024 * 1024;
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x8086, 0, atThreshold));
}

TEST(IsIntegratedGPUFromDescTest, AmdBelowThresholdIsIntegrated)
{
    constexpr uint64_t belowThreshold = (1024ULL * 1024 * 1024) - 1;
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x1002, 0, belowThreshold));
}

TEST(IsIntegratedGPUFromDescTest, AmdAtOrAboveThresholdIsDiscrete)
{
    constexpr uint64_t atThreshold = 1024ULL * 1024 * 1024;
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x1002, 0, atThreshold));
}

TEST(IsIntegratedGPUFromDescTest, NvidiaIsNeverIntegrated)
{
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x10DE, 0, 0));
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x10DE, 0, 0xFFFFFFFFFFFFFFFFULL));
}

TEST(IsIntegratedGPUFromDescTest, UnknownVendorIsNeverIntegrated)
{
    EXPECT_FALSE(isIntegratedGPUFromDesc(0x1234, 0, 0));
}

TEST(IsIntegratedGPUFromDescTest, QualcommIsIntegrated)
{
    // Adreno is always the SoC's own GPU, whatever DXGI reports as dedicated (#1263).
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x5143, 0, 0));
    EXPECT_TRUE(isIntegratedGPUFromDesc(0x5143, 0, 2ULL * 1024 * 1024 * 1024));
}

// =============================================================================
// classifyIntegrated: DXCore's answer first, the descriptor heuristic without it (#1263).
// =============================================================================

TEST(ClassifyIntegratedTest, AnAmdApuWithALargeCarveOutIsIntegratedByDXCore)
{
    // A ROG Ally / desktop APU: the heuristic's "under 1 GiB" rule reads 2 GiB as discrete.
    constexpr uint64_t carveOut = 2ULL * 1024 * 1024 * 1024;
    EXPECT_TRUE(classifyIntegrated(true, 0x1002, 0, carveOut));
}

TEST(ClassifyIntegratedTest, ASmallDiscreteGpuIsDiscreteByDXCore)
{
    // A 512 MiB AMD dGPU: the heuristic would call it integrated.
    constexpr uint64_t vram = 512ULL * 1024 * 1024;
    EXPECT_FALSE(classifyIntegrated(false, 0x1002, 0, vram));
    EXPECT_FALSE(classifyIntegrated(false, 0x8086, 0, 0)); // An Arc with no reported VRAM
}

TEST(ClassifyIntegratedTest, WithoutDXCoreTheDescriptorHeuristicDecides)
{
    EXPECT_TRUE(classifyIntegrated(std::nullopt, 0x5143, 0, 0)); // Qualcomm
    EXPECT_TRUE(classifyIntegrated(std::nullopt, 0x8086, 0, 128ULL * 1024 * 1024));
    EXPECT_FALSE(classifyIntegrated(std::nullopt, 0x10DE, 0, 0));
    EXPECT_FALSE(classifyIntegrated(std::nullopt, 0x1002, 0, 4ULL * 1024 * 1024 * 1024));
}

TEST(ClassifyIntegratedTest, ASoftwareAdapterIsNeverIntegrated)
{
    constexpr uint32_t SOFTWARE_FLAG = 2;
    EXPECT_FALSE(classifyIntegrated(true, 0x1414, SOFTWARE_FLAG, 0));
    EXPECT_FALSE(classifyIntegrated(std::nullopt, 0x5143, SOFTWARE_FLAG, 0));
}

} // namespace
} // namespace Platform
