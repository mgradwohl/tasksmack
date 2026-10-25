/// @file test_WindowsDXGIGPUProbeDXCore.cpp
/// @brief DXGIGPUProbe's DXCore reads against a fake DXCore factory (#2039): whether a GPU is
/// integrated (#1263), the NPUs and compute accelerators (#1799), and every DXCore call failing.
///
/// The probe's DXGI factory and D3DKMT calls are the fakes of WindowsDXGIFake.h, and its DXCore
/// factory the fake of WindowsDXCoreFake.h, so no test depends on this machine's adapters.

#ifdef _WIN32

#include "Mocks/WindowsDXCoreFake.h"
#include "Mocks/WindowsDXGIFake.h"
#include "Platform/GPUTypes.h"
#include "Platform/Windows/DXGIGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <dxcore_interface.h>
#pragma clang diagnostic pop
// clang-format on

namespace Platform
{
namespace
{

using namespace Platform::DXCoreFake; // NOLINT(google-build-using-namespace)
using namespace Platform::DXGIFake;   // NOLINT(google-build-using-namespace)

constexpr std::uint32_t VENDOR_NVIDIA = 0x10DE;
constexpr std::uint32_t VENDOR_INTEL = 0x8086;
constexpr std::uint64_t SHARED_MEMORY = 16ULL << 30U;

/// A discrete-looking NVIDIA adapter (8 GiB dedicated) with this LUID: the descriptor heuristic
/// calls it discrete, so only DXCore can call it integrated.
FakeAdapter nvidiaGPU(std::uint32_t luidLowPart)
{
    return makeAdapter(L"NVIDIA GeForce RTX 4090", VENDOR_NVIDIA, 0x2684, luidLowPart, std::nullopt);
}

/// A DXCore GPU adapter with this LUID whose IsIntegrated is @p integrated.
FakeCoreAdapter coreGPU(std::uint32_t luidLowPart, bool integrated)
{
    FakeCoreAdapter adapter = makeCoreAdapter(luidLowPart, {DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU});
    setProperty(adapter, DXCoreAdapterProperty::IsIntegrated, integrated);
    return adapter;
}

/// A DXCore hardware accelerator with this LUID, hardware-type attribute, name and IDs, sharing system memory.
FakeCoreAdapter coreAccelerator(std::uint32_t luidLowPart, const GUID& hardwareType, std::string_view name)
{
    FakeCoreAdapter adapter = makeCoreAdapter(luidLowPart, {hardwareType});
    setProperty(adapter, DXCoreAdapterProperty::IsHardware, true);
    setProperty(adapter, DXCoreAdapterProperty::IsIntegrated, true);
    setDescription(adapter, name);
    setProperty(adapter,
                DXCoreAdapterProperty::HardwareID,
                DXCoreHardwareID{.vendorID = VENDOR_INTEL, .deviceID = 0x7D1D, .subSysID = 0, .revision = 0});
    setProperty(adapter, DXCoreAdapterProperty::DedicatedAdapterMemory, std::uint64_t{0});
    setProperty(adapter, DXCoreAdapterProperty::SharedSystemMemory, SHARED_MEMORY);
    return adapter;
}

std::string luidId(std::uint32_t luidLowPart)
{
    return std::format("GPU_0x00000000_0x{:08X}", luidLowPart);
}

const GPUInfo* findByLuid(const std::vector<GPUInfo>& devices, std::uint32_t luidLowPart)
{
    const auto it = std::ranges::find_if(devices, [luidLowPart](const GPUInfo& device) { return device.luidId == luidId(luidLowPart); });
    return it == devices.end() ? nullptr : &*it;
}

class DXGIGPUProbeDXCoreTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dxgiState() = FakeDXGIState{};
        dxcoreState() = FakeDXCoreState{};
    }

    void TearDown() override
    {
        EXPECT_EQ(dxcoreState().liveObjects, 0) << "Every DXCore object handed out was released";
    }

    /// @p probe with the fake DXGI factory and D3DKMT calls, and a fake DXCore factory.
    static void useFakes(DXGIGPUProbe& probe)
    {
        DXGIGPUProbeTestAccessor::useFakes(probe);
        DXGIGPUProbeTestAccessor::useDXCoreFactory(probe, createFakeDXCoreFactory());
    }
};

// DXCore's IsIntegrated decides, over the descriptor heuristic, where it answers. Where DXCore has no
// adapter with the LUID, the adapter doesn't support IsIntegrated or its read fails, the heuristic
// decides. Each adapter is asked about once, and enumeration and counters agree (#1263).
TEST_F(DXGIGPUProbeDXCoreTest, DXCoreSaysWhetherAGPUIsIntegratedAndTheHeuristicCoversItsFailures)
{
    setAdapters({nvidiaGPU(0x100), nvidiaGPU(0x200), nvidiaGPU(0x300), nvidiaGPU(0x400)});
    const FakeCoreAdapter unsupported = makeCoreAdapter(0x300, {DXCORE_HARDWARE_TYPE_ATTRIBUTE_GPU});
    FakeCoreAdapter unreadable = coreGPU(0x400, true);
    unreadable.failingReads = {DXCoreAdapterProperty::IsIntegrated};
    dxcoreState().adapters = {coreGPU(0x100, true), unsupported, unreadable};

    DXGIGPUProbe probe;
    useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 4U);
    EXPECT_TRUE(findByLuid(gpus, 0x100)->isIntegrated) << "DXCore's answer";
    EXPECT_TRUE(findByLuid(gpus, 0x100)->memoryIsShared);
    EXPECT_FALSE(findByLuid(gpus, 0x200)->isIntegrated) << "DXCore has no adapter with this LUID";
    EXPECT_FALSE(findByLuid(gpus, 0x300)->isIntegrated) << "IsIntegrated unsupported";
    EXPECT_FALSE(findByLuid(gpus, 0x400)->isIntegrated) << "IsIntegrated unreadable";
    EXPECT_EQ(dxcoreState().adaptersByLuid, 3);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 4U);
    EXPECT_EQ(dxcoreState().adaptersByLuid, 3) << "Decided once per LUID";
    EXPECT_EQ(dxcoreState().liveObjects, 1) << "Only the factory is still held";
}

// A DXCore that can't look any adapter up leaves every GPU to the heuristic.
TEST_F(DXGIGPUProbeDXCoreTest, AFailingAdapterLookupLeavesTheHeuristicToDecide)
{
    setAdapters({nvidiaGPU(0x100)});
    dxcoreState().adapters = {coreGPU(0x100, true)};
    dxcoreState().failGetAdapterByLuid = true;

    DXGIGPUProbe probe;
    useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].isIntegrated);
}

// DXCore's NPU and compute-accelerator lists give the accelerators, each with its kind, name, vendor
// and memory. Unreadable properties fall back: an adapter whose IsHardware can't be read counts as
// hardware, one whose description can't be read (or sized) is "Unknown accelerator". An adapter
// without a readable LUID is left out, as is one in neither list (a GPU) (#1799).
TEST_F(DXGIGPUProbeDXCoreTest, AcceleratorsAreReadFromDXCoreAndUnreadablePropertiesFallBack)
{
    const FakeCoreAdapter npu = coreAccelerator(0x500, DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU, "Intel(R) AI Boost");
    FakeCoreAdapter compute = coreAccelerator(0x600, DXCORE_HARDWARE_TYPE_ATTRIBUTE_COMPUTE_ACCELERATOR, "Compute");
    setProperty(compute, DXCoreAdapterProperty::IsHardware, false);
    compute.failingReads = {DXCoreAdapterProperty::IsHardware, DXCoreAdapterProperty::DriverDescription};
    FakeCoreAdapter noLuid = coreAccelerator(0x700, DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU, "No LUID");
    noLuid.properties.erase(DXCoreAdapterProperty::InstanceLuid);
    FakeCoreAdapter unsized = coreAccelerator(0x800, DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU, "Unsized");
    unsized.failPropertySize = true;
    dxcoreState().adapters = {npu, compute, noLuid, unsized, coreGPU(0x900, false)};

    DXGIGPUProbe probe;
    useFakes(probe);
    const auto accelerators = probe.enumerateAccelerators();
    ASSERT_EQ(accelerators.size(), 3U);
    EXPECT_EQ(dxcoreState().adapterListsCreated, 2) << "One list per hardware type";

    const GPUInfo* boost = findByLuid(accelerators, 0x500);
    ASSERT_NE(boost, nullptr);
    EXPECT_EQ(boost->kind, DeviceKind::Npu);
    EXPECT_EQ(boost->name, "Intel(R) AI Boost") << "Up to the terminating NUL";
    EXPECT_EQ(boost->vendor, "Intel");
    EXPECT_EQ(boost->pciDeviceId, (0x7D1DU << 16U) | VENDOR_INTEL);
    EXPECT_TRUE(boost->isIntegrated);
    EXPECT_TRUE(boost->memoryIsShared);

    const GPUInfo* unknown = findByLuid(accelerators, 0x600);
    ASSERT_NE(unknown, nullptr) << "IsHardware unreadable: counted as hardware";
    EXPECT_EQ(unknown->kind, DeviceKind::ComputeAccelerator);
    EXPECT_EQ(unknown->name, "Unknown accelerator");

    const GPUInfo* unsizedInfo = findByLuid(accelerators, 0x800);
    ASSERT_NE(unsizedInfo, nullptr);
    EXPECT_EQ(unsizedInfo->name, "Unknown accelerator");

    const auto counters = probe.readAcceleratorCounters();
    ASSERT_EQ(counters.size(), 3U);
    EXPECT_EQ(counters[0].memoryTotalBytes, SHARED_MEMORY);
    EXPECT_EQ(dxcoreState().liveObjects, 1) << "Lists and adapters are released; only the factory is held";
}

// A hardware-type list DXCore can't make, and an adapter it can't hand out of a list, are skipped;
// the rest are still found.
TEST_F(DXGIGPUProbeDXCoreTest, AnUnmadeListAndAnAdapterThatCannotBeFetchedAreSkipped)
{
    dxcoreState().adapters = {
        coreAccelerator(0x500, DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU, "NPU"),
        coreAccelerator(0x600, DXCORE_HARDWARE_TYPE_ATTRIBUTE_COMPUTE_ACCELERATOR, "First"),
        coreAccelerator(0x610, DXCORE_HARDWARE_TYPE_ATTRIBUTE_COMPUTE_ACCELERATOR, "Second"),
    };
    dxcoreState().failingListAttributes = {DXCORE_HARDWARE_TYPE_ATTRIBUTE_NPU};
    dxcoreState().failGetAdapterAt = 0;

    DXGIGPUProbe probe;
    useFakes(probe);
    const auto accelerators = probe.enumerateAccelerators();
    ASSERT_EQ(accelerators.size(), 1U);
    EXPECT_EQ(accelerators[0].name, "Second");
    EXPECT_EQ(dxcoreState().adapterListsCreated, 1) << "The NPU list failed";
}

// A dxcore.dll without DXCoreCreateAdapterFactory makes no factory, so a full rescan drops the
// DXCore factory it had and classifies the new adapters by their descriptor (#1294).
TEST_F(DXGIGPUProbeDXCoreTest, ARescanThatCannotMakeADXCoreFactoryFallsBackToTheHeuristic)
{
    setAdapters({nvidiaGPU(0x100)});
    dxcoreState().adapters = {coreGPU(0x100, true), coreGPU(0x200, true)};

    DXGIGPUProbe probe;
    useFakes(probe);
    ASSERT_TRUE(probe.enumerateGPUs().at(0).isIntegrated);

    // kernel32.dll stands in for a dxcore.dll without the export; the probe frees it with its own.
    HMODULE module = LoadLibraryExW(L"kernel32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(module, nullptr);
    DXGIGPUProbeTestAccessor::useDXCoreModule(probe, module);

    setAdapters({nvidiaGPU(0x100), nvidiaGPU(0x200)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(dxcoreState().liveObjects, 0) << "The old DXCore factory was released";
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_FALSE(findByLuid(gpus, 0x100)->isIntegrated) << "Decided again, by descriptor";
    EXPECT_FALSE(findByLuid(gpus, 0x200)->isIntegrated);
    EXPECT_TRUE(probe.enumerateAccelerators().empty()) << "No DXCore factory, no accelerators";
}

} // namespace
} // namespace Platform

#endif // _WIN32
