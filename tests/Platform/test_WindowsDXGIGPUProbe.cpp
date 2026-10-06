/// @file test_WindowsDXGIGPUProbe.cpp
/// @brief Unit and smoke tests for Platform::DXGIGPUProbe
///
/// The vendor-ID/LUID-format/integrated-GPU decision logic is pure (no COM adapter
/// required) and is unit tested on every platform in WindowsMath/test_DXGIGPUProbeMath.cpp.
/// The tests here need Windows: D3DKMT fakes, DisplayDevicePower, and integration smoke
/// tests against whatever adapters are actually present on the machine running CI.

#ifdef _WIN32

#include "Platform/GPUTypes.h"
#include "Platform/Windows/DXGIAdapterLocation.h"
#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/DisplayDevicePower.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace Platform
{
namespace
{

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

// The devnode for a location: the exact function where it is known; where it isn't (NVML's busId
// unreadable), only a function unique at the bus and device, since on a multi-function device the
// first match could be another function, whose power state isn't this adapter's.
TEST(DisplayDevicePowerTest, AnUnknownFunctionMatchesOnlyAUniqueDevNode)
{
    const std::vector<PciLocation> multiFunction = {PciLocation{.bus = 0x00, .device = 0x02, .function = 0},
                                                    PciLocation{.bus = 0x01, .device = 0x00, .function = 0},
                                                    PciLocation{.bus = 0x01, .device = 0x00, .function = 1}};
    EXPECT_EQ(matchingDevNode(multiFunction, PciLocation{.bus = 0x01, .device = 0x00, .function = 1}), std::optional<std::size_t>{2});
    EXPECT_EQ(matchingDevNode(multiFunction, PciLocation{.bus = 0x01, .device = 0x00, .function = 0}), std::optional<std::size_t>{1});
    EXPECT_FALSE(matchingDevNode(multiFunction, PciLocation{.bus = 0x01, .device = 0x00, .function = std::nullopt}).has_value())
        << "Two functions fit: unknown, so the GPU counts as awake";
    EXPECT_EQ(matchingDevNode(multiFunction, PciLocation{.bus = 0x00, .device = 0x02, .function = std::nullopt}),
              std::optional<std::size_t>{0})
        << "One function fits";
    EXPECT_FALSE(matchingDevNode(multiFunction, PciLocation{.bus = 0x41, .device = 0x00, .function = std::nullopt}).has_value());
}

TEST(DisplayDevicePowerTest, AnUnknownLocationIsAwake)
{
    // Bus 0x100 and device 0x20 are outside PCI's 8-bit bus and 5-bit device ranges, so no devnode
    // can ever report them, whatever hardware runs the test: unknown counts as awake, so a GPU is
    // never left unmonitored by mistake.
    constexpr PciLocation IMPOSSIBLE{.bus = 0x100, .device = 0x20, .function = std::nullopt};
    DisplayDevicePower power;
    EXPECT_FALSE(power.isAsleep(IMPOSSIBLE));
    EXPECT_FALSE(power.isAsleep(IMPOSSIBLE)); // Cached miss
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
