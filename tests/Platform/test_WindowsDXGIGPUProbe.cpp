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
        address->FunctionNumber = 0;
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

TEST_F(AdapterPciLocationTest, ReturnsTheBusAndDeviceTheAdapterReports)
{
    fakeD3DKMT().bus = 0x41;
    fakeD3DKMT().device = 0x03;
    const LUID luid{.LowPart = 0x1234, .HighPart = 0x5};

    const auto location = adapterPciLocation(luid, FAKE_D3DKMT);

    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location.value_or(PciLocation{}), (PciLocation{.bus = 0x41, .device = 0x03}));
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
    EXPECT_EQ(second.value_or(PciLocation{}), (PciLocation{.bus = 0x02, .device = 0x01}));
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
