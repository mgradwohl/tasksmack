/// @file test_WindowsDXGIGPUProbe.cpp
/// @brief Unit and smoke tests for Platform::DXGIGPUProbe
///
/// The vendor-ID/LUID-format/integrated-GPU decision logic is pure (no COM adapter
/// required) and is unit tested directly via DXGIGPUProbeMath.h. Enumeration/counter
/// tests below are integration smoke tests against whatever adapters are actually
/// present on the machine running CI.

#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/DXGIAdapterLocation.h"
#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/DisplayDevicePower.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace Platform
{
namespace
{

// =============================================================================
// vendorIdToName: pure lookup, no hardware required.
// =============================================================================

// =============================================================================
// adapterPciLocation: the D3DKMT address query, against fake kernel-mode calls (#1091)
// =============================================================================

// What the fakes saw and should answer. A plain global: the D3DKMT function pointers take no
// user data.
struct FakeD3DKMT
{
    NTSTATUS openStatus = 0;
    NTSTATUS queryStatus = 0;
    UINT bus = 0;
    UINT device = 0;
    UINT function = 0;
    UINT adapterTypeValue = 0; // D3DKMT_ADAPTERTYPE::Value answered to KMTQAITYPE_ADAPTERTYPE (#1251)
    D3DKMT_HANDLE handle = 0x40;
    LUID openedLuid{};
    KMTQUERYADAPTERINFOTYPE queriedType{};
    UINT queriedSize = 0;
    D3DKMT_HANDLE queriedHandle = 0;
    D3DKMT_HANDLE closedHandle = 0;
    int closeCount = 0;
};

FakeD3DKMT& fakeD3DKMT()
{
    static FakeD3DKMT state;
    return state;
}

NTSTATUS APIENTRY fakeOpenAdapterFromLuid(const D3DKMT_OPENADAPTERFROMLUID* open)
{
    auto& fake = fakeD3DKMT();
    fake.openedLuid = open->AdapterLuid;
    if (fake.openStatus == 0)
    {
        // The real call fills hAdapter through its nominally const argument.
        const_cast<D3DKMT_OPENADAPTERFROMLUID*>(open)->hAdapter = fake.handle; // NOLINT(cppcoreguidelines-pro-type-const-cast)
    }
    return fake.openStatus;
}

NTSTATUS APIENTRY fakeQueryAdapterInfo(const D3DKMT_QUERYADAPTERINFO* query)
{
    auto& fake = fakeD3DKMT();
    fake.queriedType = query->Type;
    fake.queriedSize = query->PrivateDriverDataSize;
    fake.queriedHandle = query->hAdapter;
    if (fake.queryStatus == 0 && query->Type == KMTQAITYPE_ADAPTERADDRESS && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERADDRESS))
    {
        auto* address = static_cast<D3DKMT_ADAPTERADDRESS*>(query->pPrivateDriverData);
        address->BusNumber = fake.bus;
        address->DeviceNumber = fake.device;
        address->FunctionNumber = fake.function;
    }
    if (fake.queryStatus == 0 && query->Type == KMTQAITYPE_ADAPTERTYPE && query->PrivateDriverDataSize == sizeof(D3DKMT_ADAPTERTYPE))
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - D3DKMT_ADAPTERTYPE is a union of its bitfields and Value
        static_cast<D3DKMT_ADAPTERTYPE*>(query->pPrivateDriverData)->Value = fake.adapterTypeValue;
    }
    return fake.queryStatus;
}

NTSTATUS APIENTRY fakeCloseAdapter(const D3DKMT_CLOSEADAPTER* close)
{
    auto& fake = fakeD3DKMT();
    fake.closedHandle = close->hAdapter;
    ++fake.closeCount;
    return 0;
}

constexpr D3DKMTAdapterFunctions FAKE_D3DKMT{
    .openAdapterFromLuid = fakeOpenAdapterFromLuid,
    .queryAdapterInfo = fakeQueryAdapterInfo,
    .closeAdapter = fakeCloseAdapter,
};

class AdapterPciLocationTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakeD3DKMT() = FakeD3DKMT{};
    }
};

TEST_F(AdapterPciLocationTest, ReturnsTheBusDeviceAndFunctionTheAdapterReports)
{
    fakeD3DKMT().bus = 0x41;
    fakeD3DKMT().device = 0x03;
    fakeD3DKMT().function = 0x01;
    const LUID luid{.LowPart = 0x1234, .HighPart = 0x5};

    const auto location = adapterPciLocation(luid, FAKE_D3DKMT);

    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location.value_or(PciLocation{}), (PciLocation{.bus = 0x41, .device = 0x03, .function = 0x01}));
    // It opened the adapter the LUID names, asked that adapter for its address, and closed it.
    EXPECT_EQ(fakeD3DKMT().openedLuid.LowPart, 0x1234U);
    EXPECT_EQ(fakeD3DKMT().openedLuid.HighPart, 0x5);
    EXPECT_EQ(fakeD3DKMT().queriedType, KMTQAITYPE_ADAPTERADDRESS);
    EXPECT_EQ(fakeD3DKMT().queriedSize, sizeof(D3DKMT_ADAPTERADDRESS));
    EXPECT_EQ(fakeD3DKMT().queriedHandle, fakeD3DKMT().handle);
    EXPECT_EQ(fakeD3DKMT().closedHandle, fakeD3DKMT().handle);
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
}

TEST_F(AdapterPciLocationTest, DistinctAdaptersGetDistinctLocations)
{
    fakeD3DKMT().bus = 0x01;
    const auto first = adapterPciLocation(LUID{}, FAKE_D3DKMT);
    fakeD3DKMT().bus = 0x02;
    fakeD3DKMT().device = 0x01;
    const auto second = adapterPciLocation(LUID{}, FAKE_D3DKMT);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(first.value_or(PciLocation{}), second.value_or(PciLocation{}));
    EXPECT_EQ(second.value_or(PciLocation{}), (PciLocation{.bus = 0x02, .device = 0x01, .function = 0}));
}

TEST_F(AdapterPciLocationTest, NoLocationWhenTheAdapterCannotBeOpened)
{
    fakeD3DKMT().openStatus = static_cast<NTSTATUS>(0xC0000225L); // STATUS_NOT_FOUND
    fakeD3DKMT().bus = 0x41;

    EXPECT_FALSE(adapterPciLocation(LUID{}, FAKE_D3DKMT).has_value());
    EXPECT_EQ(fakeD3DKMT().closeCount, 0); // Nothing was opened, so nothing to close
}

TEST_F(AdapterPciLocationTest, NoLocationWhenTheAddressQueryFailsAndTheAdapterIsStillClosed)
{
    fakeD3DKMT().queryStatus = static_cast<NTSTATUS>(0xC00000BBL); // STATUS_NOT_SUPPORTED
    fakeD3DKMT().bus = 0x41;

    EXPECT_FALSE(adapterPciLocation(LUID{}, FAKE_D3DKMT).has_value());
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
    EXPECT_EQ(fakeD3DKMT().closedHandle, fakeD3DKMT().handle);
}

// =============================================================================
// adapterKind / shouldListAdapter: indirect-display adapters are not GPUs (#1251)
// =============================================================================

// D3DKMT_ADAPTERTYPE::Value for an adapter with these type bits.
UINT adapterType(bool render, bool software, bool indirectDisplay)
{
    // NOLINTBEGIN(cppcoreguidelines-pro-type-union-access) - D3DKMT_ADAPTERTYPE is a union of its bitfields and Value
    D3DKMT_ADAPTERTYPE type{};
    type.RenderSupported = render ? 1U : 0U;
    type.SoftwareDevice = software ? 1U : 0U;
    type.IndirectDisplayDevice = indirectDisplay ? 1U : 0U;
    return type.Value;
    // NOLINTEND(cppcoreguidelines-pro-type-union-access)
}

// Whether the adapter the fake D3DKMT calls describe is listed, decided as DXGIGPUProbe does.
bool listedByFake(const LUID& luid)
{
    const auto kind = adapterKind(luid, FAKE_D3DKMT);
    return shouldListAdapter(false, kind.has_value() ? std::optional{adapterTypeBits(*kind)} : std::nullopt);
}

TEST_F(AdapterPciLocationTest, AdapterKindQueriesTheAdapterTypeAndClosesTheAdapter)
{
    fakeD3DKMT().adapterTypeValue = adapterType(true, false, true);
    const LUID luid{.LowPart = 0x5AB51A6F, .HighPart = 0};

    const auto kind = adapterKind(luid, FAKE_D3DKMT);

    ASSERT_TRUE(kind.has_value());
    EXPECT_TRUE(adapterTypeBits(kind.value_or(D3DKMT_ADAPTERTYPE{})).indirectDisplayDevice);
    EXPECT_FALSE(adapterTypeBits(kind.value_or(D3DKMT_ADAPTERTYPE{})).softwareDevice);
    EXPECT_EQ(fakeD3DKMT().openedLuid.LowPart, 0x5AB51A6FU);
    EXPECT_EQ(fakeD3DKMT().queriedType, KMTQAITYPE_ADAPTERTYPE);
    EXPECT_EQ(fakeD3DKMT().queriedSize, sizeof(D3DKMT_ADAPTERTYPE));
    EXPECT_EQ(fakeD3DKMT().queriedHandle, fakeD3DKMT().handle);
    EXPECT_EQ(fakeD3DKMT().closedHandle, fakeD3DKMT().handle);
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
}

TEST_F(AdapterPciLocationTest, AnIndirectDisplayAdapterIsSkipped)
{
    // A DisplayLink dock's adapter: DXGI lists it under the name of the iGPU it renders on (#1251).
    fakeD3DKMT().adapterTypeValue = adapterType(false, false, true);
    EXPECT_FALSE(listedByFake(LUID{}));
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
}

TEST_F(AdapterPciLocationTest, ARenderAdapterIsKept)
{
    fakeD3DKMT().adapterTypeValue = adapterType(true, false, false);
    EXPECT_TRUE(listedByFake(LUID{}));
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
}

TEST_F(AdapterPciLocationTest, AFailedTypeQueryKeepsTheAdapterAndStillClosesIt)
{
    fakeD3DKMT().queryStatus = static_cast<NTSTATUS>(0xC00000BBL); // STATUS_NOT_SUPPORTED
    fakeD3DKMT().adapterTypeValue = adapterType(false, false, true);

    EXPECT_FALSE(adapterKind(LUID{}, FAKE_D3DKMT).has_value());
    EXPECT_EQ(fakeD3DKMT().closeCount, 1);
    EXPECT_EQ(fakeD3DKMT().closedHandle, fakeD3DKMT().handle);
    EXPECT_TRUE(listedByFake(LUID{})) << "dropping a real GPU is worse than a possible duplicate";
}

TEST_F(AdapterPciLocationTest, AnAdapterThatCannotBeOpenedHasNoKindAndIsKept)
{
    fakeD3DKMT().openStatus = static_cast<NTSTATUS>(0xC0000225L); // STATUS_NOT_FOUND
    EXPECT_FALSE(adapterKind(LUID{}, FAKE_D3DKMT).has_value());
    EXPECT_EQ(fakeD3DKMT().closeCount, 0); // Nothing was opened, so nothing to close
    EXPECT_TRUE(listedByFake(LUID{}));
}

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

// #1317: the id depends on the adapter, not on where DXGI lists it or the LUID it has now.
TEST(StableAdapterIdTest, TheIdIgnoresTheLuidWhenThereIsAPciLocation)
{
    const PciLocation location{.bus = 0x01, .device = 0x00, .function = 0};
    EXPECT_EQ(stableAdapterId(location, 0x10DE, 0x2684, 0, 0x200, {}), stableAdapterId(location, 0x10DE, 0x2684, 0, 0x210, {}));
    EXPECT_NE(stableAdapterId(location, 0x10DE, 0x2684, 0, 0x200, {}), stableAdapterId(location, 0x10DE, 0x2782, 0, 0x200, {}))
        << "A different card later fitted at the same location is a different GPU";
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

// =============================================================================
// DisplayDevicePower: whether a GPU is asleep, from the PnP manager, never from the GPU (#1265).
// =============================================================================

TEST(DisplayDevicePowerTest, D1ToD3AreAsleepD0AndUnspecifiedAreAwake)
{
    EXPECT_FALSE(isAsleepDevicePowerState(PowerDeviceUnspecified));
    EXPECT_FALSE(isAsleepDevicePowerState(PowerDeviceD0));
    EXPECT_TRUE(isAsleepDevicePowerState(PowerDeviceD1));
    EXPECT_TRUE(isAsleepDevicePowerState(PowerDeviceD2));
    EXPECT_TRUE(isAsleepDevicePowerState(PowerDeviceD3));
    EXPECT_FALSE(isAsleepDevicePowerState(PowerDeviceMaximum));
}

TEST(DisplayDevicePowerTest, PciAddressCarriesTheDeviceInItsHighWordAndTheFunctionInItsLow)
{
    // DEVPKEY_Device_Address for PCI is (device << 16) | function.
    EXPECT_EQ(pciLocationFromDevNode(0x01, 0x00000000), (PciLocation{.bus = 0x01, .device = 0x00, .function = 0}));
    EXPECT_EQ(pciLocationFromDevNode(0x00, 0x00020000), (PciLocation{.bus = 0x00, .device = 0x02, .function = 0}));
    EXPECT_EQ(pciLocationFromDevNode(0x41, 0x001F0003), (PciLocation{.bus = 0x41, .device = 0x1F, .function = 3}));
}

TEST(DisplayDevicePowerTest, AnUnknownLocationIsAwake)
{
    // No display adapter sits at bus 255, device 31: unknown counts as awake, so a GPU is never
    // left unmonitored by mistake.
    DisplayDevicePower power;
    EXPECT_FALSE(power.isAsleep(PciLocation{.bus = 0xFF, .device = 0x1F, .function = std::nullopt}));
    EXPECT_FALSE(power.isAsleep(PciLocation{.bus = 0xFF, .device = 0x1F, .function = std::nullopt})); // Cached miss
}

TEST(DisplayDevicePowerTest, QueryingRealAdaptersDoesNotFail)
{
    // Smoke test on real hardware: an adapter that DXGI lists and that reports a PCI location
    // answers without throwing. (Whether it is asleep depends on the machine, so only the call is
    // checked.)
    DXGIGPUProbe probe;
    DisplayDevicePower power;
    for (const auto& gpu : probe.enumerateGPUs())
    {
        if (gpu.pciLocation.has_value())
        {
            [[maybe_unused]] const bool asleep = power.isAsleep(*gpu.pciLocation);
        }
    }
    SUCCEED();
}

// =============================================================================
// Smoke Tests (real DXGI adapters, whatever this machine has)
// =============================================================================

TEST(DXGIGPUProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ DXGIGPUProbe probe; });
}

TEST(DXGIGPUProbeTest, EnumerateGPUsReturnsWellFormedData)
{
    DXGIGPUProbe probe;
    auto gpus = probe.enumerateGPUs();

    std::unordered_set<std::string> ids;
    for (const auto& gpu : gpus)
    {
        EXPECT_FALSE(gpu.id.empty());
        EXPECT_FALSE(gpu.luidId.empty());
        EXPECT_TRUE(ids.insert(gpu.id).second) << "GPU ids should be unique";
    }
}

TEST(DXGIGPUProbeTest, ReadGPUCountersMatchesEnumeration)
{
    DXGIGPUProbe probe;
    auto gpus = probe.enumerateGPUs();
    auto counters = probe.readGPUCounters();

    EXPECT_EQ(gpus.size(), counters.size());
    for (const auto& counter : counters)
    {
        // DXGI alone reads neither (#1245).
        EXPECT_FALSE(counter.utilizationAvailable);
        EXPECT_FALSE(counter.memoryAvailable);
    }
}

TEST(DXGIGPUProbeTest, ReadProcessGPUCountersIsAlwaysEmpty)
{
    // DXGI provides no per-process GPU metrics; that's PDH/NVML's job.
    DXGIGPUProbe probe;
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST(DXGIGPUProbeTest, CapabilitiesReflectDXGILimitations)
{
    DXGIGPUProbe probe;
    const auto caps = probe.capabilities();

    // DXGI never provides these, regardless of whether the factory initialized.
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    // supportsMultiGPU is only true once the DXGI factory is initialized; a machine
    // without a usable DXGI runtime is the only case where this would be false.
}

} // namespace
} // namespace Platform

#endif // _WIN32
