/// @file test_WindowsGPURescan.cpp
/// @brief Re-detecting GPUs on Windows while running (#1294): DXGIGPUProbe and WindowsGPUProbe
/// rescans against a fake DXGI factory, fake D3DKMT calls and a fake NVML backend.
///
/// A real driver reset, eGPU hot-plug or hybrid dGPU wake can't be staged in a test, so these drive
/// the same code paths through the fakes: a factory that stops being current, an NVML reading that
/// reports the GPU lost, and a GPU whose PnP power state changes from asleep to awake.

#ifdef _WIN32

#include "Mocks/WindowsDXGIFake.h"
#include "Mocks/WindowsNVMLFake.h"
#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"
#include "Platform/Windows/AdapterPerfDataMath.h"
#include "Platform/Windows/DXGIAdapterLocation.h"
#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/NVMLGPUProbe.h"
#include "Platform/Windows/PDHGPUProbe.h"
#include "Platform/Windows/PDHGPUProbeImpl.h"
#include "Platform/Windows/WindowsDriverNodes.h"
#include "Platform/Windows/WindowsGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform
{

/// Test-only accessor (a friend of WindowsGPUProbe): reaches its sub-probes so a test can back them
/// with fakes, and drops PDH, whose real counters would describe this machine's adapters, or puts a
/// per-process PDH probe backed by fakes in its place.
struct WindowsGPUProbeTestAccessor
{
    static DXGIGPUProbe& dxgi(WindowsGPUProbe& probe)
    {
        return *probe.m_DXGIProbe;
    }
    static NVMLGPUProbe& nvml(WindowsGPUProbe& probe)
    {
        return *probe.m_NVMLProbe;
    }
    static void dropPDH(WindowsGPUProbe& probe)
    {
        probe.m_PDHProbe.reset();
        probe.m_PDHAdapterProbe.reset();
    }
    static void setProcessPDH(WindowsGPUProbe& probe, std::unique_ptr<PDHGPUProbe> pdh)
    {
        probe.m_PDHProbe = std::move(pdh);
    }
    static void setAdapterPDH(WindowsGPUProbe& probe, std::unique_ptr<PDHGPUProbe> pdh)
    {
        probe.m_PDHAdapterProbe = std::move(pdh);
    }
    /// The PnP device nodes driver versions are read from (#1799), in place of this machine's.
    static void setDriverNodes(WindowsGPUProbe& probe, std::vector<WindowsGraphics::DriverNode> (*list)())
    {
        probe.m_ListDriverNodes = list;
    }
    /// The kernel adapter calls the perf-data sensors are read through (#2586), in place of gdi32's.
    static void setPerfFunctions(WindowsGPUProbe& probe, const D3DKMTAdapterFunctions& functions)
    {
        *probe.m_PerfFunctions = functions;
    }
    /// Whether a GPU at a PCI location is asleep, in place of this machine's PnP power states (#1265).
    static void setAsleep(WindowsGPUProbe& probe, std::function<bool(const PciLocation&)> isAsleep)
    {
        probe.m_IsAsleep = std::move(isAsleep);
    }
};

namespace
{

using namespace Platform::DXGIFake; // NOLINT(google-build-using-namespace)
using namespace Platform::NVMLFake; // NOLINT(google-build-using-namespace)

constexpr std::uint32_t VENDOR_NVIDIA = 0x10DE;
constexpr std::uint32_t VENDOR_INTEL = 0x8086;
constexpr std::uint32_t DEVICE_RTX = 0x2684;

FakeAdapter intelIGPU()
{
    return makeAdapter(
        L"Intel(R) UHD Graphics", VENDOR_INTEL, 0x9A49, 0x100, PciLocation{.bus = 0x00, .device = 0x02, .function = 0}, 128ULL << 20U);
}

FakeAdapter nvidiaGPU(std::uint32_t luidLowPart, std::uint32_t bus)
{
    return makeAdapter(
        L"NVIDIA GeForce RTX 4090", VENDOR_NVIDIA, DEVICE_RTX, luidLowPart, PciLocation{.bus = bus, .device = 0x00, .function = 0});
}

/// NVML device @p index: the card at @p bus, with this UUID and temperature.
void setNVMLDevice(unsigned int index, const std::string& uuid, unsigned int bus, unsigned int temperatureC)
{
    auto& device = deviceData(index);
    device = FakeDeviceData{};
    device.uuid = uuid;
    device.pciBus = bus;
    device.pciDeviceId = (DEVICE_RTX << 16U) | VENDOR_NVIDIA;
    device.temperatureC = temperatureC;
}

const GPUInfo* findByLuid(const std::vector<GPUInfo>& gpus, std::uint32_t luidLowPart)
{
    const auto it = std::ranges::find_if(
        gpus, [luidLowPart](const GPUInfo& gpu) { return gpu.luidId == std::format("GPU_0x00000000_0x{:08X}", luidLowPart); });
    return it == gpus.end() ? nullptr : &*it;
}

class WindowsGPURescanTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        dxgiState() = FakeDXGIState{};
        fakeState() = FakeNvmlState{};
    }

    /// A WindowsGPUProbe whose DXGI and NVML are the fakes (NVML started) and that has no PDH.
    static void useFakes(WindowsGPUProbe& probe)
    {
        DXGIGPUProbeTestAccessor::useFakes(WindowsGPUProbeTestAccessor::dxgi(probe));
        NVMLGPUProbeTestAccessor::inject(WindowsGPUProbeTestAccessor::nvml(probe), NVMLGPUProbeTestAccessor::fullFakeFunctions(), true);
        WindowsGPUProbeTestAccessor::dropPDH(probe);
        WindowsGPUProbeTestAccessor::setDriverNodes(probe, [] { return std::vector<WindowsGraphics::DriverNode>{}; });
        // The perf-data sensors through the fake D3DKMT too (#2586), and no adapter asleep unless a
        // test says so, rather than this machine's PnP power states.
        WindowsGPUProbeTestAccessor::setPerfFunctions(probe, fakeD3DKMTFunctions());
        WindowsGPUProbeTestAccessor::setAsleep(probe, [](const PciLocation&) { return false; });
    }
};

// =============================================================================
// DXGIGPUProbe
// =============================================================================

// A factory lists the adapters present when it was made. A quick rescan never looks; a full one
// with the factory still current reports nothing; once an adapter is added the factory is no longer
// current, and a full rescan replaces it and reports a change, after which the new adapter is listed.
TEST_F(WindowsGPURescanTest, ANonCurrentFactoryIsReplacedAndTheAddedAdapterListed)
{
    setAdapters({intelIGPU()});
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "The factory is current";

    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U) << "The old factory still lists the old set";
    EXPECT_EQ(probe.readGPUCounters().size(), 1U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    const int factoriesBefore = dxgiState().factoriesCreated;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(dxgiState().factoriesCreated, factoriesBefore + 1);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    const GPUInfo* nvidia = findByLuid(gpus, 0x200);
    ASSERT_NE(nvidia, nullptr);
    EXPECT_EQ(nvidia->vendor, "NVIDIA");
    EXPECT_EQ(nvidia->pciLocation.value_or(PciLocation{}).bus, 0x01U);
    EXPECT_EQ(probe.readGPUCounters().size(), 2U) << "Counters follow the new factory too";
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "The new factory is current";
}

// Removing an adapter is picked up the same way, and the per-LUID decisions (listed? integrated?)
// are made afresh, since a driver reset can bring an adapter back under a LUID seen before (#1251,
// #1263).
TEST_F(WindowsGPURescanTest, ARemovedAdapterIsDroppedAndPerLuidDecisionsAreMadeAgain)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    const int opensAfterFirst = dxgiState().adapterOpens;
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    EXPECT_EQ(dxgiState().adapterOpens - opensAfterFirst, 2) << "Once decided, only each adapter's PCI location is queried";

    setAdapters({intelIGPU()});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const int opensBefore = dxgiState().adapterOpens;
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "Intel");
    EXPECT_TRUE(gpus[0].isIntegrated);
    EXPECT_EQ(dxgiState().adapterOpens - opensBefore, 2) << "Its adapter type is queried again, as well as its location";
}

// A replacement factory that can't be made keeps the old one (its adapters beat none) and reports
// no change; the next full rescan tries again.
TEST_F(WindowsGPURescanTest, AFailedFactoryReplacementKeepsTheOldFactoryAndRetries)
{
    setAdapters({intelIGPU()});
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});

    dxgiState().failFactoryCreation = true;
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);

    dxgiState().failFactoryCreation = false;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 2U);
}

// Without a first factory the probe lists and reads nothing and reports no capabilities; a full
// rescan that can make one then starts it, and without dxcore.dll no DXCore factory is made (#2039).
TEST_F(WindowsGPURescanTest, WithoutAFactoryNothingIsListedUntilARescanMakesOne)
{
    setAdapters({intelIGPU()});
    dxgiState().failFactoryCreation = true;
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    EXPECT_TRUE(probe.enumerateGPUs().empty());
    EXPECT_TRUE(probe.readGPUCounters().empty());
    EXPECT_TRUE(probe.enumerateAccelerators().empty());
    EXPECT_FALSE(probe.capabilities().supportsMultiGPU);
    EXPECT_FALSE(DXGIGPUProbeTestAccessor::createDXCoreFactory(probe));

    dxgiState().failFactoryCreation = false;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_TRUE(probe.capabilities().supportsMultiGPU);
}

// An adapter slot that enumerates without an adapter ends the walk: the adapters before it are
// listed and read, none after it.
TEST_F(WindowsGPURescanTest, AnEmptyAdapterSlotEndsTheWalk)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01), nvidiaGPU(0x300, 0x02)});
    dxgiState().nullAdapterAt = 1;
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].vendor, "Intel");
    EXPECT_EQ(probe.readGPUCounters().size(), 1U);
}

// An adapter's id is its own, not its place in DXGI's list (#1317). Removing the adapter listed
// first used to renumber the one after it ("GPU1" became "GPU0"), so its history and chart state
// moved to the removed card's, and an adapter added later took the next free number. Now the
// remaining adapter keeps its id, an added adapter in another slot gets one no adapter had before,
// and the same card back under a new LUID (a driver reset) is still the same GPU. (The id names a
// slot and model, so an identical card swapped into the same slot would continue the old history.)
TEST_F(WindowsGPURescanTest, AnAdapterKeepsItsIdWhenOneListedBeforeItIsRemoved)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    const std::string intelId = findByLuid(before, 0x100)->id;
    const std::string nvidiaId = findByLuid(before, 0x200)->id;
    ASSERT_NE(intelId, nvidiaId);

    // The first-listed adapter is removed (disabled in Device Manager, say).
    setAdapters({nvidiaGPU(0x200, 0x01)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto removed = probe.enumerateGPUs();
    ASSERT_EQ(removed.size(), 1U);
    EXPECT_EQ(removed[0].id, nvidiaId) << "The remaining adapter keeps its id";
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuId, nvidiaId) << "Counters carry the same id enumeration reported";

    // A new adapter is plugged in: it gets an id of its own, not the removed adapter's.
    setAdapters({nvidiaGPU(0x200, 0x01), nvidiaGPU(0x300, 0x41)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto added = probe.enumerateGPUs();
    ASSERT_EQ(added.size(), 2U);
    const GPUInfo* newcomer = findByLuid(added, 0x300);
    ASSERT_NE(newcomer, nullptr);
    EXPECT_NE(newcomer->id, intelId);
    EXPECT_NE(newcomer->id, nvidiaId);
    EXPECT_EQ(findByLuid(added, 0x200)->id, nvidiaId);

    // A driver reset brings the first NVIDIA card back under a new LUID, listed after the other.
    setAdapters({nvidiaGPU(0x300, 0x41), nvidiaGPU(0x210, 0x01)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto reset = probe.enumerateGPUs();
    ASSERT_EQ(reset.size(), 2U);
    EXPECT_EQ(findByLuid(reset, 0x210)->id, nvidiaId) << "Same card, same PCI location: same GPU";
    EXPECT_EQ(findByLuid(reset, 0x300)->id, newcomer->id);
}

// An adapter with no PCI location (a remote or virtual adapter) is named by its LUID, and two
// adapters at one PCI location never share an id.
TEST_F(WindowsGPURescanTest, AnAdapterWithoutAUniquePciLocationIsNamedByItsLuid)
{
    setAdapters(
        {makeAdapter(L"Remote Adapter", VENDOR_INTEL, 0x1234, 0x400, std::nullopt), nvidiaGPU(0x200, 0x01), nvidiaGPU(0x500, 0x01)});
    DXGIGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);
    EXPECT_EQ(findByLuid(gpus, 0x400)->id, adapterLuidId(0, 0x400));
    EXPECT_EQ(findByLuid(gpus, 0x200)->id,
              adapterPciId(PciLocation{.bus = 0x01, .device = 0x00, .function = 0}, VENDOR_NVIDIA, DEVICE_RTX));
    EXPECT_EQ(findByLuid(gpus, 0x500)->id, adapterLuidId(0, 0x500)) << "The PCI id is taken: the LUID names it";
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);
    for (std::size_t i = 0; i < gpus.size(); ++i)
    {
        EXPECT_EQ(counters[i].gpuId, gpus[i].id);
    }
}

// =============================================================================
// WindowsGPUProbe
// =============================================================================

// An NVIDIA GPU plugged in (an eGPU) makes the factory non-current: the full rescan reports a change,
// and the re-enumeration lists the new adapter, restarts NVML (whose device list is fixed when it
// starts) and matches the new NVML device to it by PCI location. Unplugging it is picked up the same
// way.
TEST_F(WindowsGPURescanTest, AnAddedOrRemovedNVIDIAAdapterIsListedAndRestartsNVML)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);

    WindowsGPUProbe probe;
    useFakes(probe);
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 0);

    // Plugged in: a second NVIDIA adapter, and NVML lists it once restarted.
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01), nvidiaGPU(0x300, 0x41)});
    fakeState().deviceCount = 2;
    setNVMLDevice(1, "GPU-external", 0x41, 70);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto plugged = probe.enumerateGPUs();
    EXPECT_EQ(fakeState().initCallCount, 1) << "NVML restarted for the new NVIDIA adapter";
    ASSERT_EQ(plugged.size(), 3U);
    const GPUInfo* external = findByLuid(plugged, 0x300);
    ASSERT_NE(external, nullptr);
    EXPECT_TRUE(external->sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature) << "Matched to its NVML device";

    const auto counters = probe.readGPUCounters();
    const auto externalCounters = std::ranges::find(counters, external->id, &GPUCounters::gpuId);
    ASSERT_NE(externalCounters, counters.end());
    EXPECT_EQ(externalCounters->temperatureC, 70);

    // Unplugged again.
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto unplugged = probe.enumerateGPUs();
    EXPECT_EQ(fakeState().initCallCount, 2);
    ASSERT_EQ(unplugged.size(), 2U);
    EXPECT_EQ(findByLuid(unplugged, 0x300), nullptr);
}

// An adapter set change that leaves the NVIDIA adapters alone (a dock's display adapter, say) doesn't
// restart NVML, which could wake a sleeping dGPU.
TEST_F(WindowsGPURescanTest, AChangeToOtherAdaptersLeavesNVMLRunning)
{
    setAdapters({nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);

    WindowsGPUProbe probe;
    useFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);

    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 2U);
    EXPECT_EQ(fakeState().initCallCount, 0);
}

// A reading that reports the GPU lost (a driver reset) re-initialises NVML at the next full rescan.
// NVML may then number its devices differently; the re-enumeration matches them to the adapters by
// PCI location again, so each adapter keeps its own card's sensors.
TEST_F(WindowsGPURescanTest, AGpuLostErrorReinitialisesNVMLAndRematchesByPciLocation)
{
    setAdapters({nvidiaGPU(0x200, 0x01), nvidiaGPU(0x300, 0x41)});
    fakeState().deviceCount = 2;
    setNVMLDevice(0, "GPU-first", 0x01, 50);
    setNVMLDevice(1, "GPU-second", 0x41, 70);

    WindowsGPUProbe probe;
    useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    const std::string firstId = findByLuid(gpus, 0x200)->id;
    const std::string secondId = findByLuid(gpus, 0x300)->id;
    const auto temperatureOf = [&probe](const std::string& id)
    {
        const auto counters = probe.readGPUCounters();
        const auto it = std::ranges::find(counters, id, &GPUCounters::gpuId);
        return it == counters.end() || !it->temperatureAvailable ? -1 : it->temperatureC;
    };
    EXPECT_EQ(temperatureOf(firstId), 50);
    EXPECT_EQ(temperatureOf(secondId), 70);

    fakeState().lostDevices.insert(0);
    static_cast<void>(probe.readGPUCounters());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "Re-initialising waits for a full rescan";

    // After the reset NVML lists the cards the other way round.
    fakeState().lostDevices.clear();
    setNVMLDevice(0, "GPU-second", 0x41, 70);
    setNVMLDevice(1, "GPU-first", 0x01, 50);
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 1);
    const auto after = probe.enumerateGPUs();
    ASSERT_EQ(after.size(), 2U);
    EXPECT_EQ(temperatureOf(firstId), 50);
    EXPECT_EQ(temperatureOf(secondId), 70);
}

// A LUID can be reused: DXGI may list a different NVIDIA card under the LUID an old one had. The
// LUIDs alone then compare equal, and NVML kept the old card's devices and handles. The adapter's
// identity (its slot-and-model id and PCI ids) is compared too, so the replacement restarts NVML and
// is matched to its own NVML device.
TEST_F(WindowsGPURescanTest, AnotherNVIDIACardUnderAReusedLuidRestartsNVML)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-old", 0x01, 50);

    WindowsGPUProbe probe;
    useFakes(probe);
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    const std::string oldId = findByLuid(before, 0x200)->id;

    // Replaced by another model, listed under the same LUID.
    constexpr std::uint32_t DEVICE_OTHER = 0x2782;
    setAdapters(
        {intelIGPU(),
         makeAdapter(
             L"NVIDIA GeForce RTX 4070 Ti", VENDOR_NVIDIA, DEVICE_OTHER, 0x200, PciLocation{.bus = 0x01, .device = 0x00, .function = 0})});
    setNVMLDevice(0, "GPU-new", 0x01, 65);
    deviceData(0).name = "NVIDIA GeForce RTX 4070 Ti";
    deviceData(0).pciDeviceId = (DEVICE_OTHER << 16U) | VENDOR_NVIDIA;
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto after = probe.enumerateGPUs();
    EXPECT_EQ(fakeState().initCallCount, 1) << "The replacement restarts NVML";
    ASSERT_EQ(after.size(), 2U);
    const GPUInfo* replacement = findByLuid(after, 0x200);
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->id, oldId);
    EXPECT_TRUE(replacement->sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature) << "Matched to the new NVML device";
    const auto counters = probe.readGPUCounters();
    const auto it = std::ranges::find(counters, replacement->id, &GPUCounters::gpuId);
    ASSERT_NE(it, counters.end());
    EXPECT_EQ(it->temperatureC, 65);
}

// A driver reset is seen by both probes in one full rescan: NVML restarts for the lost GPU, and
// DXGI's factory is no longer current because the GPU came back under a new LUID. The
// re-enumeration that follows sees the changed NVIDIA LUID set, but NVML, started after the reset,
// already lists the GPU, so it isn't shut down and started a second time. A later change it hasn't
// restarted for (an eGPU plugged in) still restarts it.
TEST_F(WindowsGPURescanTest, ADriverResetSeenByBothProbesRestartsNVMLOnce)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);

    WindowsGPUProbe probe;
    useFakes(probe);
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    const std::string nvidiaId = findByLuid(before, 0x200)->id;

    // The reset: readings report the GPU lost, and it comes back under a new LUID.
    fakeState().lostDevices.insert(0);
    static_cast<void>(probe.readGPUCounters());
    fakeState().lostDevices.clear();
    setAdapters({intelIGPU(), nvidiaGPU(0x210, 0x01)});
    const int shutdownsBefore = fakeState().shutdownCallCount;
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto after = probe.enumerateGPUs();
    EXPECT_EQ(fakeState().initCallCount, 1) << "One NVML restart for one driver reset";
    EXPECT_EQ(fakeState().shutdownCallCount, shutdownsBefore + 1);
    ASSERT_EQ(after.size(), 2U);
    const GPUInfo* nvidia = findByLuid(after, 0x210);
    ASSERT_NE(nvidia, nullptr);
    EXPECT_EQ(nvidia->id, nvidiaId) << "Same card, same PCI location: same GPU";
    EXPECT_TRUE(nvidia->sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature) << "Still matched to its NVML device";

    // A later NVIDIA change NVML hasn't restarted for restarts it as before.
    setAdapters({intelIGPU(), nvidiaGPU(0x210, 0x01), nvidiaGPU(0x300, 0x41)});
    fakeState().deviceCount = 2;
    setNVMLDevice(1, "GPU-external", 0x41, 70);
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(probe.enumerateGPUs().size(), 3U);
    EXPECT_EQ(fakeState().initCallCount, 2);
}

// NVML that is installed but not running while an NVIDIA adapter is present (it failed to start, or
// to restart after a reset) is tried again at each full rescan, and the GPUs are re-enumerated once
// it starts.
TEST_F(WindowsGPURescanTest, NVMLThatFailedToStartIsRetriedWhileAnNVIDIAAdapterIsPresent)
{
    setAdapters({nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);

    WindowsGPUProbe probe;
    DXGIGPUProbeTestAccessor::useFakes(WindowsGPUProbeTestAccessor::dxgi(probe));
    NVMLGPUProbeTestAccessor::inject(WindowsGPUProbeTestAccessor::nvml(probe), NVMLGPUProbeTestAccessor::fullFakeFunctions(), false);
    WindowsGPUProbeTestAccessor::dropPDH(probe);
    const auto without = probe.enumerateGPUs();
    ASSERT_EQ(without.size(), 1U);
    EXPECT_FALSE(without[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);

    fakeState().initResult = NVML_ERROR_DRIVER_NOT_LOADED;
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 1);

    fakeState().initResult = NVML_SUCCESS;
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    const auto with = probe.enumerateGPUs();
    ASSERT_EQ(with.size(), 1U);
    EXPECT_TRUE(with[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
}

// A GPU asleep at enumeration (a hybrid laptop's dGPU) gets the NVML probe's general sensor set, as
// it isn't woken to find its own. Once its PnP power state says it is awake, a quick rescan -- which
// asks only that -- reports a change, and the re-enumeration gives it its own sensors.
TEST_F(WindowsGPURescanTest, AWokenAdapterGetsItsOwnSensorsOnAQuickRescan)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);
    deviceData(0).fanOk = false; // A laptop dGPU: no fan reading

    WindowsGPUProbe probe;
    useFakes(probe);
    bool asleep = true;
    NVMLGPUProbeTestAccessor::setAsleep(WindowsGPUProbeTestAccessor::nvml(probe), [&asleep](const PciLocation&) { return asleep; });

    const auto sleeping = probe.enumerateGPUs();
    const GPUInfo* dGPU = findByLuid(sleeping, 0x200);
    ASSERT_NE(dGPU, nullptr);
    EXPECT_TRUE(dGPU->sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed) << "The probe's general set while unknown";
    fakeState().deviceQueries.clear();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));

    asleep = false;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(fakeState().deviceQueries[0], 0) << "Finding out it woke didn't ask the GPU";
    const auto awake = probe.enumerateGPUs();
    dGPU = findByLuid(awake, 0x200);
    ASSERT_NE(dGPU, nullptr);
    ASSERT_TRUE(dGPU->sensorCapabilities.has_value());
    EXPECT_FALSE(dGPU->sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    EXPECT_TRUE(dGPU->sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
}

// NVML's video-engine utilization reaches the adapter the GPU tab draws (#1485): each NVIDIA
// adapter, matched to its NVML device by PCI location (NVML lists the second DXGI adapter first,
// #1091), gets its own device's encoder/decoder readings and their availability, and its sensor
// set says whether it has the engines at all. A card without them (NVML_ERROR_NOT_SUPPORTED) shows
// gaps, not 0%; the iGPU, which NVML doesn't cover, has no encoder/decoder capability.
TEST_F(WindowsGPURescanTest, NVMLEncoderAndDecoderReachTheMatchedAdapter)
{
    setAdapters({intelIGPU(), nvidiaGPU(0x200, 0x01), nvidiaGPU(0x300, 0x41)});
    fakeState().deviceCount = 2;
    setNVMLDevice(0, "GPU-second", 0x41, 70);
    deviceData(0).encoderResult = NVML_ERROR_NOT_SUPPORTED;
    deviceData(0).decoderResult = NVML_ERROR_NOT_SUPPORTED;
    setNVMLDevice(1, "GPU-first", 0x01, 50);
    deviceData(1).encoderPercent = 55;
    deviceData(1).decoderPercent = 9;

    WindowsGPUProbe probe;
    useFakes(probe);
    EXPECT_TRUE(probe.capabilities().hasEncoderDecoder);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 3U);
    const GPUInfo* iGPU = findByLuid(gpus, 0x100);
    const GPUInfo* first = findByLuid(gpus, 0x200);
    const GPUInfo* second = findByLuid(gpus, 0x300);
    ASSERT_NE(iGPU, nullptr);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_FALSE(iGPU->sensorCapabilities.value_or(GPUCapabilities{}).hasEncoderDecoder);
    EXPECT_TRUE(first->sensorCapabilities.value_or(GPUCapabilities{}).hasEncoderDecoder);
    EXPECT_FALSE(second->sensorCapabilities.value_or(GPUCapabilities{}).hasEncoderDecoder);

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 3U);
    const auto counterFor = [&counters](const std::string& id)
    {
        return std::ranges::find(counters, id, &GPUCounters::gpuId);
    };
    const auto withEngines = counterFor(first->id);
    const auto withoutEngines = counterFor(second->id);
    ASSERT_NE(withEngines, counters.end());
    ASSERT_NE(withoutEngines, counters.end());
    EXPECT_TRUE(withEngines->encoderAvailable);
    EXPECT_DOUBLE_EQ(withEngines->encoderUtilPercent, 55.0);
    EXPECT_TRUE(withEngines->decoderAvailable);
    EXPECT_DOUBLE_EQ(withEngines->decoderUtilPercent, 9.0);
    EXPECT_FALSE(withoutEngines->encoderAvailable);
    EXPECT_FALSE(withoutEngines->decoderAvailable);
}

// =============================================================================
// Sensors from D3DKMT adapter perf data for the GPUs NVML doesn't cover (#2586)
// =============================================================================

/// A discrete AMD card at PCI bus 3, with a temperature, fan and clock in its perf data.
FakeAdapter amdGPU()
{
    FakeAdapter adapter =
        makeAdapter(L"AMD Radeon RX 7800 XT", 0x1002, 0x747E, 0x300, PciLocation{.bus = 0x03, .device = 0x00, .function = 0});
    adapter.perf = FakeAdapterPerf{.temperatureDeciCelsius = 612,
                                   .fanRpm = 1200,
                                   .maxFanRpm = 3300,
                                   .engineFrequencyHz = 2'124'000'000ULL,
                                   .maxEngineFrequencyHz = 2'430'000'000ULL};
    return adapter;
}

/// The counter for the adapter with this LUID, or end.
auto counterOf(const std::vector<GPUCounters>& counters, const std::vector<GPUInfo>& gpus, std::uint32_t luidLowPart)
{
    const GPUInfo* gpu = findByLuid(gpus, luidLowPart);
    return gpu == nullptr ? counters.end() : std::ranges::find(counters, gpu->id, &GPUCounters::gpuId);
}

// A hybrid laptop: the NVIDIA GPU keeps NVML's sensors; the Intel iGPU, which no NVML device covers,
// gets the temperature and clock its kernel adapter reports -- and no fan, since it reports no fan
// maximum. Each sample reads them again, scaled into the counter.
TEST_F(WindowsGPURescanTest, AGpuWithoutAnNVMLDeviceGetsItsSensorsFromTheKernelAdapter)
{
    FakeAdapter igpu = intelIGPU();
    igpu.perf =
        FakeAdapterPerf{.temperatureDeciCelsius = 455, .engineFrequencyHz = 1'850'000'000ULL, .maxEngineFrequencyHz = 2'250'000'000ULL};
    setAdapters({igpu, nvidiaGPU(0x200, 0x01)});
    fakeState().deviceCount = 1;
    setNVMLDevice(0, "GPU-internal", 0x01, 50);

    WindowsGPUProbe probe;
    useFakes(probe);
    EXPECT_TRUE(probe.capabilities().hasTemperature) << "The probe can read a temperature before the first enumeration says whose";
    EXPECT_TRUE(probe.capabilities().hasClockSpeeds);

    const auto gpus = probe.enumerateGPUs();
    const GPUInfo* intel = findByLuid(gpus, 0x100);
    const GPUInfo* nvidia = findByLuid(gpus, 0x200);
    ASSERT_NE(intel, nullptr);
    ASSERT_NE(nvidia, nullptr);
    ASSERT_TRUE(intel->sensorCapabilities.has_value());
    const GPUCapabilities intelSensors = intel->sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(intelSensors.hasTemperature);
    EXPECT_TRUE(intelSensors.hasClockSpeeds);
    EXPECT_FALSE(intelSensors.hasFanSpeed) << "No fan maximum reported";
    EXPECT_FALSE(intelSensors.hasPowerMetrics) << "Perf data has power as a percentage only";
    EXPECT_FALSE(intelSensors.hasEncoderDecoder);
    EXPECT_TRUE(nvidia->sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature) << "NVML's, as before";
    EXPECT_EQ(dxgiState().perfQueries, 1) << "The NVIDIA adapter, matched to NVML, isn't asked";

    const auto counters = probe.readGPUCounters();
    const auto intelCounter = counterOf(counters, gpus, 0x100);
    const auto nvidiaCounter = counterOf(counters, gpus, 0x200);
    ASSERT_NE(intelCounter, counters.end());
    ASSERT_NE(nvidiaCounter, counters.end());
    EXPECT_TRUE(intelCounter->temperatureAvailable);
    EXPECT_EQ(intelCounter->temperatureC, 46) << "45.5 °C rounds up";
    EXPECT_TRUE(intelCounter->gpuClockAvailable);
    EXPECT_EQ(intelCounter->gpuClockMHz, 1850U);
    EXPECT_EQ(intelCounter->fanSpeedMaxRaw, 0U);
    EXPECT_FALSE(intelCounter->suspended);
    EXPECT_TRUE(nvidiaCounter->temperatureAvailable);
    EXPECT_EQ(nvidiaCounter->temperatureC, 50) << "NVML's reading, not the kernel adapter's";
    EXPECT_EQ(dxgiState().perfQueries, 2) << "One per sample for the Intel adapter alone";
}

// A read that fails this sample (the driver mid-reset, say) is a gap for each of the adapter's
// sensors, never a logged error or a real-looking 0; the next sample that reads fills them again.
// A sensor the adapter didn't claim at enumeration stays unread even when a sample reports it.
TEST_F(WindowsGPURescanTest, AFailedKernelAdapterReadIsAGapThisSample)
{
    FakeAdapter igpu = intelIGPU();
    igpu.perf =
        FakeAdapterPerf{.temperatureDeciCelsius = 455, .engineFrequencyHz = 1'850'000'000ULL, .maxEngineFrequencyHz = 2'250'000'000ULL};
    setAdapters({igpu});

    WindowsGPUProbe probe;
    useFakes(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_NE(findByLuid(gpus, 0x100), nullptr);
    FakeAdapter* adapter = adapterWithLuid(LUID{.LowPart = 0x100, .HighPart = 0});
    ASSERT_NE(adapter, nullptr);

    ASSERT_TRUE(adapter->perf.has_value());
    const FakeAdapterPerf perf = adapter->perf.value_or(FakeAdapterPerf{});
    adapter->perf.reset();
    const auto failed = probe.readGPUCounters();
    const auto gap = counterOf(failed, gpus, 0x100);
    ASSERT_NE(gap, failed.end());
    EXPECT_FALSE(gap->temperatureAvailable);
    EXPECT_FALSE(gap->gpuClockAvailable);
    EXPECT_EQ(gap->temperatureC, 0);
    EXPECT_FALSE(gap->suspended) << "A failed read is not a sleeping GPU";

    adapter->perf = perf;
    adapter->perf->temperatureDeciCelsius = 500;
    adapter->perf->fanRpm = 1000;
    adapter->perf->maxFanRpm = 3000; // Reported now, but the adapter claimed no fan at enumeration
    const auto read = probe.readGPUCounters();
    const auto reading = counterOf(read, gpus, 0x100);
    ASSERT_NE(reading, read.end());
    EXPECT_TRUE(reading->temperatureAvailable);
    EXPECT_EQ(reading->temperatureC, 50);
    EXPECT_TRUE(reading->gpuClockAvailable);
    EXPECT_EQ(reading->fanSpeedMaxRaw, 0U) << "Masked: no fan series appears mid-run";
}

// A discrete AMD GPU asleep at enumeration isn't asked for its perf data, which could wake it, so it
// has no sensors until a quick rescan finds it awake and the re-enumeration reads it; once asleep
// again, a sample leaves it alone and marks it suspended.
TEST_F(WindowsGPURescanTest, AGpuAsleepAtEnumerationIsReadOnceAQuickRescanFindsItAwake)
{
    setAdapters({intelIGPU(), amdGPU()});

    WindowsGPUProbe probe;
    useFakes(probe);
    bool asleep = true;
    WindowsGPUProbeTestAccessor::setAsleep(probe, [&asleep](const PciLocation& location) { return asleep && location.bus == 0x03; });

    const auto sleeping = probe.enumerateGPUs();
    const GPUInfo* amd = findByLuid(sleeping, 0x300);
    ASSERT_NE(amd, nullptr);
    ASSERT_TRUE(amd->sensorCapabilities.has_value());
    EXPECT_FALSE(hasAnyAdapterPerfSensor(amd->sensorCapabilities.value_or(GPUCapabilities{}))) << "Unknown while asleep: nothing is drawn";
    EXPECT_EQ(dxgiState().perfQueries, 1) << "The Intel adapter alone was asked (and reports nothing)";
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    asleep = false;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(dxgiState().perfQueries, 1) << "Finding out it woke didn't ask the GPU";
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "Reported once";
    const auto awake = probe.enumerateGPUs();
    amd = findByLuid(awake, 0x300);
    ASSERT_NE(amd, nullptr);
    ASSERT_TRUE(amd->sensorCapabilities.has_value());
    const GPUCapabilities amdSensors = amd->sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(amdSensors.hasTemperature);
    EXPECT_TRUE(amdSensors.hasFanSpeed);
    EXPECT_TRUE(amdSensors.hasClockSpeeds);
    const auto counters = probe.readGPUCounters();
    const auto reading = counterOf(counters, awake, 0x300);
    ASSERT_NE(reading, counters.end());
    EXPECT_TRUE(reading->temperatureAvailable);
    EXPECT_EQ(reading->temperatureC, 61);
    EXPECT_EQ(reading->fanSpeedRaw, 1200U);
    EXPECT_EQ(reading->fanSpeedMaxRaw, 3300U);
    EXPECT_EQ(reading->gpuClockMHz, 2124U);
    EXPECT_FALSE(reading->suspended);

    asleep = true;
    const int queriesBefore = dxgiState().perfQueries;
    const auto dozing = probe.readGPUCounters();
    const auto gap = counterOf(dozing, awake, 0x300);
    ASSERT_NE(gap, dozing.end());
    EXPECT_TRUE(gap->suspended);
    EXPECT_FALSE(gap->temperatureAvailable);
    EXPECT_FALSE(gap->gpuClockAvailable);
    EXPECT_EQ(gap->fanSpeedMaxRaw, 0U);
    EXPECT_EQ(dxgiState().perfQueries, queriesBefore) << "A sleeping GPU is left alone";
}

// =============================================================================
// Per-process samples of adapters not listed as GPUs (#1826)
// =============================================================================

/// One instance a fake PDH counter array returns: its name and value (a percentage for an engine,
/// bytes for process memory).
struct FakePDHSample
{
    std::wstring name;
    double value = 0.0;
};

/// The fake per-process PDH query: the "GPU Engine" and dedicated "GPU Process Memory" instances,
/// each read through the counter handle the probe was given for it.
struct FakeProcessPDH
{
    std::vector<FakePDHSample> engines;
    std::vector<FakePDHSample> dedicatedMemory;
    PDH_HCOUNTER engineCounter = nullptr;
    PDH_HCOUNTER dedicatedCounter = nullptr;
    // The adapter query's (fakeAdapterPDHProbe(), #1799): its own "GPU Engine" instances and the
    // "GPU Adapter Memory" shared-usage ones.
    std::vector<FakePDHSample> adapterEngines;
    std::vector<FakePDHSample> adapterSharedMemory;
    PDH_HCOUNTER adapterEngineCounter = nullptr;
    PDH_HCOUNTER adapterSharedCounter = nullptr;
    std::uintptr_t nextHandle = 1;
};

FakeProcessPDH& processPDH()
{
    static FakeProcessPDH state;
    return state;
}

PDH_STATUS WINAPI fakePdhOpenQuery(LPCWSTR /*source*/, DWORD_PTR /*userData*/, PDH_HQUERY* query)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a fake query handle the fakes never dereference
    *query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakePdhCloseQuery(PDH_HQUERY /*query*/)
{
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakePdhAddEnglishCounter(PDH_HQUERY /*query*/, LPCWSTR /*path*/, DWORD_PTR /*userData*/, PDH_HCOUNTER* counter)
{
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a fake counter handle the fakes never dereference
    *counter = reinterpret_cast<PDH_HCOUNTER>(processPDH().nextHandle++);
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakePdhCollectQueryData(PDH_HQUERY /*query*/)
{
    return ERROR_SUCCESS;
}

PDH_STATUS WINAPI fakePdhGetFormattedCounterArray(
    PDH_HCOUNTER counter, DWORD format, LPDWORD bufferSize, LPDWORD itemCount, PPDH_FMT_COUNTERVALUE_ITEM_W buffer)
{
    auto& state = processPDH();
    std::vector<FakePDHSample> none;
    std::vector<FakePDHSample>* list = &none;
    if (counter == state.engineCounter)
    {
        list = &state.engines;
    }
    else if (counter == state.dedicatedCounter)
    {
        list = &state.dedicatedMemory;
    }
    else if (counter == state.adapterEngineCounter)
    {
        list = &state.adapterEngines;
    }
    else if (counter == state.adapterSharedCounter)
    {
        list = &state.adapterSharedMemory;
    }
    std::vector<FakePDHSample>& samples = *list;
    const auto needed = static_cast<DWORD>(sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) * samples.size());
    if (!samples.empty() && (*bufferSize < needed || buffer == nullptr))
    {
        *bufferSize = needed;
        return static_cast<PDH_STATUS>(PDH_MORE_DATA);
    }
    *itemCount = static_cast<DWORD>(samples.size());
    std::size_t index = 0;
    while (index < samples.size())
    {
        auto& item = buffer[index];
        item.szName = samples[index].name.data();
        item.FmtValue.CStatus = ERROR_SUCCESS;
        if ((format & PDH_FMT_LARGE) != 0)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_LARGE selects largeValue
            item.FmtValue.largeValue = static_cast<LONGLONG>(samples[index].value);
        }
        else
        {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - PDH_FMT_DOUBLE selects doubleValue
            item.FmtValue.doubleValue = samples[index].value;
        }
        ++index;
    }
    return ERROR_SUCCESS;
}

/// A per-process PDH probe whose query is the fake above, already warmed up.
std::unique_ptr<PDHGPUProbe> fakeProcessPDHProbe()
{
    auto impl = std::make_unique<PDHGPUProbe::Impl>();
    impl->role = PDHGPUProbe::Role::Process;
    impl->pdhOpenQuery = fakePdhOpenQuery;
    impl->pdhCloseQuery = fakePdhCloseQuery;
    impl->pdhAddEnglishCounter = fakePdhAddEnglishCounter;
    impl->pdhCollectQueryData = fakePdhCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakePdhGetFormattedCounterArray;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a fake query handle the fakes never dereference
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    impl->ensureCounters();
    impl->warmedUp = true;
    processPDH().engineCounter = impl->utilizationCounter;
    processPDH().dedicatedCounter = impl->dedicatedMemoryCounter;
    return std::make_unique<PDHGPUProbe>(std::move(impl));
}

/// An adapter PDH probe (#1799) whose query is the fake above, already warmed up: adapter
/// utilization from FakeProcessPDH::adapterEngines, memory in use from adapterSharedMemory.
std::unique_ptr<PDHGPUProbe> fakeAdapterPDHProbe()
{
    auto impl = std::make_unique<PDHGPUProbe::Impl>();
    impl->role = PDHGPUProbe::Role::Adapter;
    impl->pdhOpenQuery = fakePdhOpenQuery;
    impl->pdhCloseQuery = fakePdhCloseQuery;
    impl->pdhAddEnglishCounter = fakePdhAddEnglishCounter;
    impl->pdhCollectQueryData = fakePdhCollectQueryData;
    impl->pdhGetFormattedCounterArray = fakePdhGetFormattedCounterArray;
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - a fake query handle the fakes never dereference
    impl->query = reinterpret_cast<PDH_HQUERY>(static_cast<std::uintptr_t>(1));
    impl->initialized = true;
    impl->ensureCounters();
    impl->warmedUp = true;
    processPDH().adapterEngineCounter = impl->utilizationCounter;
    processPDH().adapterSharedCounter = impl->adapterSharedCounter;
    return std::make_unique<PDHGPUProbe>(std::move(impl));
}

constexpr std::uint32_t LUID_IGPU = 0x100;
constexpr std::uint32_t LUID_BASIC_RENDER = 0x180;
constexpr std::uint32_t LUID_NVIDIA = 0x200;
constexpr std::uint32_t LUID_NPU = 0x18103; // Intel AI Boost on a Core Ultra 7 255H: MCDM, never listed by DXGI

/// The Basic Render Driver: DXGI lists it with the software flag, so TaskSmack doesn't.
FakeAdapter basicRenderDriver()
{
    FakeAdapter adapter = makeAdapter(L"Microsoft Basic Render Driver", 0x1414, 0x8C, LUID_BASIC_RENDER, std::nullopt, 0);
    adapter.desc.Flags = DXGI_ADAPTER_FLAG_SOFTWARE;
    return adapter;
}

std::wstring engineInstance(int pid, std::uint32_t luidLowPart, std::wstring_view engineType)
{
    return std::format(L"pid_{}_luid_0x00000000_0x{:08X}_phys_0_eng_0_engtype_{}", pid, luidLowPart, engineType);
}

std::wstring processMemoryInstance(int pid, std::uint32_t luidLowPart)
{
    return std::format(L"pid_{}_luid_0x00000000_0x{:08X}_phys_0", pid, luidLowPart);
}

std::string pdhLuid(std::uint32_t luidLowPart)
{
    return std::format("GPU_0x00000000_0x{:08X}", luidLowPart);
}

const ProcessGPUCounters* findProcess(const std::vector<ProcessGPUCounters>& counters, int pid, std::uint32_t luidLowPart)
{
    const std::string luid = pdhLuid(luidLowPart);
    const auto it = std::ranges::find_if(
        counters, [pid, &luid](const ProcessGPUCounters& counter) { return counter.pid == pid && counter.gpuId == luid; });
    return it == counters.end() ? nullptr : &*it;
}

class WindowsGPUProcessLuidFilterTest : public WindowsGPURescanTest
{
  protected:
    void SetUp() override
    {
        WindowsGPURescanTest::SetUp();
        processPDH() = FakeProcessPDH{};
        // pid 10 uses the iGPU, and the NPU harder (as "Compute", the NPU's engine type); pid 11 only
        // the NPU; pid 12 only the Basic Render Driver; pid 13 the NVIDIA GPU, once it is present.
        processPDH().engines = {
            {.name = engineInstance(10, LUID_IGPU, L"3D"), .value = 20.0},
            {.name = engineInstance(10, LUID_NPU, L"Compute"), .value = 80.0},
            {.name = engineInstance(11, LUID_NPU, L"Compute"), .value = 60.0},
            {.name = engineInstance(12, LUID_BASIC_RENDER, L"3D"), .value = 30.0},
            {.name = engineInstance(13, LUID_NVIDIA, L"3D"), .value = 40.0},
        };
        processPDH().dedicatedMemory = {
            {.name = processMemoryInstance(10, LUID_IGPU), .value = 4096.0},
            {.name = processMemoryInstance(10, LUID_NPU), .value = 8192.0},
            {.name = processMemoryInstance(11, LUID_NPU), .value = 8192.0},
        };
    }

    /// A WindowsGPUProbe with fake DXGI and NVML (as useFakes()) and the fake per-process PDH query.
    static void useFakesWithProcessPDH(WindowsGPUProbe& probe)
    {
        useFakes(probe);
        WindowsGPUProbeTestAccessor::setProcessPDH(probe, fakeProcessPDHProbe());
    }
};

// Per-process GPU % and GPU memory count only the adapters listed as GPUs (#1826). The NPU's samples
// (an MCDM adapter DXGI never lists, engine type "Compute") and the Basic Render Driver's (listed by
// DXGI, skipped by TaskSmack) are kept apart, per process and adapter LUID, so neither is folded into
// a process's GPU figures nor reaches ProcessModel as an unknown "GPU_0x..." device. The listed
// GPU's samples are unchanged.
TEST_F(WindowsGPUProcessLuidFilterTest, SamplesOfAdaptersNotListedAsGPUsAreKeptApart)
{
    setAdapters({intelIGPU(), basicRenderDriver()});
    WindowsGPUProbe probe;
    useFakesWithProcessPDH(probe);
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U) << "The Basic Render Driver isn't listed";
    ASSERT_EQ(gpus[0].luidId, pdhLuid(LUID_IGPU));

    const auto counters = probe.readProcessGPUCounters();

    ASSERT_EQ(counters.size(), 1U);
    const ProcessGPUCounters* iGPU = findProcess(counters, 10, LUID_IGPU);
    ASSERT_NE(iGPU, nullptr);
    EXPECT_DOUBLE_EQ(iGPU->gpuUtilPercent, 20.0) << "The iGPU's own figure, not the NPU's 80%";
    EXPECT_EQ(iGPU->gpuMemoryBytes, 4096U) << "Without the NPU's memory";
    EXPECT_EQ(iGPU->activeEngines, std::vector<std::string>{"3D"});
    for (const auto& counter : counters)
    {
        EXPECT_EQ(counter.gpuId, gpus[0].luidId) << "pid " << counter.pid << ": only a listed GPU's LUID";
        EXPECT_NE(counter.pid, 11) << "An NPU-only process has no GPU figures";
        EXPECT_NE(counter.pid, 12) << "Nor a Basic Render Driver-only one";
    }

    // The other adapters' samples, with their own figures: pid 10's and 11's on the NPU, 12's on the
    // Basic Render Driver (13's NVIDIA GPU isn't present, so its sample is there too).
    const auto other = probe.otherAdapterUsage();
    const auto otherFor = [&other](int pid, std::uint32_t luidLowPart)
    {
        const std::string luid = pdhLuid(luidLowPart);
        const auto it = std::ranges::find_if(
            other, [pid, &luid](const OtherAdapterProcessUsage& usage) { return usage.pid == pid && usage.adapterLuid == luid; });
        return it == other.end() ? nullptr : &*it;
    };
    EXPECT_EQ(other.size(), 4U);
    const OtherAdapterProcessUsage* npuAndGPU = otherFor(10, LUID_NPU);
    ASSERT_NE(npuAndGPU, nullptr);
    EXPECT_DOUBLE_EQ(npuAndGPU->utilPercent, 80.0);
    EXPECT_EQ(npuAndGPU->dedicatedMemoryBytes, 8192U);
    EXPECT_EQ(npuAndGPU->activeEngines, std::vector<std::string>{"Compute"});
    const OtherAdapterProcessUsage* npuOnly = otherFor(11, LUID_NPU);
    ASSERT_NE(npuOnly, nullptr);
    EXPECT_DOUBLE_EQ(npuOnly->utilPercent, 60.0);
    const OtherAdapterProcessUsage* basicRender = otherFor(12, LUID_BASIC_RENDER);
    ASSERT_NE(basicRender, nullptr);
    EXPECT_DOUBLE_EQ(basicRender->utilPercent, 30.0);
    EXPECT_NE(otherFor(13, LUID_NVIDIA), nullptr);
    EXPECT_EQ(otherFor(10, LUID_IGPU), nullptr) << "A listed GPU's sample isn't in the other adapters'";
}

// Before the first enumeration no adapter is listed, so no sample counts as a GPU's: a GPU's LUID
// can't be told from the NPU's yet. Every sample is kept apart instead.
TEST_F(WindowsGPUProcessLuidFilterTest, NoSampleIsAGPUsBeforeTheFirstEnumeration)
{
    setAdapters({intelIGPU()});
    WindowsGPUProbe probe;
    useFakesWithProcessPDH(probe);

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
    EXPECT_EQ(probe.otherAdapterUsage().size(), 5U);
}

// The filter follows the adapter set: a GPU hot-plugged in (picked up by a full rescan and the
// re-enumeration it asks for) has its processes' samples counted from then on, and once it is
// removed they are kept apart again. The NPU's never count.
TEST_F(WindowsGPUProcessLuidFilterTest, TheListedSetFollowsARescan)
{
    setAdapters({intelIGPU()});
    WindowsGPUProbe probe;
    useFakesWithProcessPDH(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    EXPECT_EQ(findProcess(probe.readProcessGPUCounters(), 13, LUID_NVIDIA), nullptr) << "Not listed yet";

    setAdapters({intelIGPU(), nvidiaGPU(LUID_NVIDIA, 0x01)});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    auto counters = probe.readProcessGPUCounters();
    const ProcessGPUCounters* nvidia = findProcess(counters, 13, LUID_NVIDIA);
    ASSERT_NE(nvidia, nullptr) << "Listed after the rescan";
    EXPECT_DOUBLE_EQ(nvidia->gpuUtilPercent, 40.0);
    EXPECT_NE(findProcess(counters, 10, LUID_IGPU), nullptr);
    EXPECT_EQ(findProcess(counters, 11, LUID_NPU), nullptr);
    EXPECT_EQ(counters.size(), 2U);
    EXPECT_EQ(probe.otherAdapterUsage().size(), 3U) << "The NPU's two samples and the Basic Render Driver's";

    setAdapters({intelIGPU()});
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full));
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    counters = probe.readProcessGPUCounters();
    EXPECT_EQ(findProcess(counters, 13, LUID_NVIDIA), nullptr) << "Kept apart again once it is removed";
    EXPECT_EQ(counters.size(), 1U);
    EXPECT_EQ(probe.otherAdapterUsage().size(), 4U);
}

// =============================================================================
// NPUs and compute accelerators from DXCore, and PnP driver versions (#1799)
// =============================================================================

constexpr std::uint32_t LUID_ACCELERATOR = 0x300;
constexpr std::uint64_t GIB = 1ULL << 30U;
const PciLocation NPU_LOCATION{.bus = 0x00, .device = 0x0B, .function = 0};

/// Intel AI Boost on a Core Ultra 7 255H: an MCDM NPU at 00:0b.0, which DXGI never lists.
FakeAccelerator intelNPU(std::uint32_t luidLowPart = LUID_NPU)
{
    return makeAccelerator("Intel(R) AI Boost", VENDOR_INTEL, 0x7D1D, luidLowPart, NPU_LOCATION);
}

/// A non-NPU compute accelerator with memory of its own.
FakeAccelerator computeAccelerator()
{
    FakeAccelerator accelerator = makeAccelerator(
        "Contoso Inference Accelerator", 0x1234, 0x5678, LUID_ACCELERATOR, PciLocation{.bus = 3, .device = 0, .function = 0}, false);
    accelerator.record.dedicatedAdapterMemory = 8 * GIB;
    return accelerator;
}

/// The PnP device nodes: the Intel iGPU's and the NPU's, each with its driver version, and another
/// node with the NPU's ids at another location, which mustn't be taken for it.
std::vector<WindowsGraphics::DriverNode> fakeDriverNodes()
{
    const auto node = [](const char* hardwareId, DWORD address, const char* driverVersion)
    {
        return WindowsGraphics::DriverNode{
            .hardwareId = hardwareId,
            .bus = 0,
            .address = address,
            .driverVersion = driverVersion,
            .driverDate = std::nullopt,
        };
    };
    return {
        node("PCI\\VEN_8086&DEV_9A49&SUBSYS_0CE51028&REV_01", 0x00020000, "32.0.101.6881"),
        node("PCI\\VEN_8086&DEV_7D1D&SUBSYS_0CE51028&REV_05", 0x000B0000, "32.0.100.4778"),
        node("PCI\\VEN_8086&DEV_7D1D&SUBSYS_00000000&REV_05", 0x000C0000, "1.0.0.0"),
    };
}

const GPUInfo* findById(const std::vector<GPUInfo>& adapters, const std::string& id)
{
    const auto it = std::ranges::find(adapters, id, &GPUInfo::id);
    return it == adapters.end() ? nullptr : &*it;
}

const GPUCounters* findCounters(const std::vector<GPUCounters>& counters, const std::string& id)
{
    const auto it = std::ranges::find(counters, id, &GPUCounters::gpuId);
    return it == counters.end() ? nullptr : &*it;
}

class WindowsGPUAcceleratorTest : public WindowsGPUProcessLuidFilterTest
{
  protected:
    /// A WindowsGPUProbe with fake DXGI, NVML, DXCore accelerators, PnP nodes and per-process PDH.
    static void useAcceleratorFakes(WindowsGPUProbe& probe)
    {
        useFakesWithProcessPDH(probe);
        DXGIGPUProbeTestAccessor::useFakeAccelerators(WindowsGPUProbeTestAccessor::dxgi(probe));
        WindowsGPUProbeTestAccessor::setDriverNodes(probe, fakeDriverNodes);
    }
};

// DXCore's NPU and compute accelerator are listed after DXGI's GPUs, with their kind, a stable
// PCI id and LUID as a GPU has, and no sensors; an adapter DXGI already lists isn't listed again
// whatever DXCore calls it. Every adapter's driver version is its PnP node's: the Intel GPU's and the
// NPU's, matched by PCI location, which were "Unknown" before.
TEST_F(WindowsGPUAcceleratorTest, DXCoreAcceleratorsAreListedWithTheirKindAndPnPDriverVersions)
{
    setAdapters({intelIGPU(), basicRenderDriver()});
    const FakeAccelerator iGPUAsAccelerator =
        makeAccelerator("Intel(R) UHD Graphics", VENDOR_INTEL, 0x9A49, LUID_IGPU, std::nullopt, false);
    dxgiState().accelerators = {intelNPU(), iGPUAsAccelerator, computeAccelerator()};
    WindowsGPUProbe probe;
    useAcceleratorFakes(probe);

    const auto adapters = probe.enumerateGPUs();

    ASSERT_EQ(adapters.size(), 3U) << "The iGPU, the NPU and the compute accelerator";
    const GPUInfo& iGPU = adapters[0];
    EXPECT_EQ(iGPU.kind, DeviceKind::Gpu);
    EXPECT_EQ(iGPU.luidId, pdhLuid(LUID_IGPU));
    EXPECT_EQ(iGPU.driverVersion, "32.0.101.6881") << "A non-NVIDIA GPU's driver version, from PnP";

    const GPUInfo& npu = adapters[1];
    EXPECT_EQ(npu.kind, DeviceKind::Npu);
    EXPECT_EQ(npu.id, "PCI_00:0B.0_8086:7D1D");
    EXPECT_EQ(npu.luidId, pdhLuid(LUID_NPU));
    EXPECT_EQ(npu.name, "Intel(R) AI Boost");
    EXPECT_EQ(npu.vendor, "Intel");
    EXPECT_EQ(npu.driverVersion, "32.0.100.4778") << "The node at the NPU's location, not the other with its ids";
    EXPECT_TRUE(npu.memoryIsShared);
    EXPECT_EQ(npu.pciLocation, std::optional<PciLocation>(NPU_LOCATION));
    GPUCapabilities unknown; // What a missing sensor set would read as: fails the check below
    unknown.hasTemperature = true;
    const GPUCapabilities sensors = npu.sensorCapabilities.value_or(unknown);
    EXPECT_TRUE(npu.sensorCapabilities.has_value());
    EXPECT_FALSE(sensors.hasTemperature || sensors.hasPowerMetrics || sensors.hasClockSpeeds || sensors.hasFanSpeed ||
                 sensors.hasEncoderDecoder)
        << "No sensors";

    const GPUInfo& accelerator = adapters[2];
    EXPECT_EQ(accelerator.kind, DeviceKind::ComputeAccelerator);
    EXPECT_EQ(accelerator.id, "PCI_03:00.0_1234:5678");
    EXPECT_FALSE(accelerator.memoryIsShared) << "Its own dedicated memory";
    EXPECT_EQ(accelerator.driverVersion, "Unknown") << "No PnP node for it";

    EXPECT_EQ(std::ranges::count(adapters, pdhLuid(LUID_IGPU), &GPUInfo::luidId), 1) << "Listed once, by DXGI";
}

// The accelerators' counters carry their kind and memory size, and PDH's adapter utilization and
// memory in use are matched to them by LUID, as a GPU's are: the NPU's 35% lands on the NPU, not
// the iGPU, whose own 20% is unchanged.
TEST_F(WindowsGPUAcceleratorTest, AnAcceleratorsUtilizationAndMemoryComeFromPDHByLuid)
{
    setAdapters({intelIGPU()});
    dxgiState().accelerators = {intelNPU(), computeAccelerator()};
    processPDH().adapterEngines = {
        {.name = engineInstance(10, LUID_IGPU, L"3D"), .value = 20.0},
        {.name = engineInstance(10, LUID_NPU, L"Compute"), .value = 35.0},
    };
    processPDH().adapterSharedMemory = {
        {.name = std::format(L"luid_0x00000000_0x{:08X}_phys_0", LUID_IGPU), .value = 1.0e9},
        {.name = std::format(L"luid_0x00000000_0x{:08X}_phys_0", LUID_NPU), .value = 8192.0},
    };
    WindowsGPUProbe probe;
    useAcceleratorFakes(probe);
    WindowsGPUProbeTestAccessor::setAdapterPDH(probe, fakeAdapterPDHProbe());
    const auto adapters = probe.enumerateGPUs();
    ASSERT_EQ(adapters.size(), 3U);

    const auto counters = probe.readGPUCounters();

    ASSERT_EQ(counters.size(), 3U);
    const GPUCounters* iGPU = findCounters(counters, adapters[0].id);
    ASSERT_NE(iGPU, nullptr);
    EXPECT_EQ(iGPU->kind, DeviceKind::Gpu);
    EXPECT_TRUE(iGPU->utilizationAvailable);
    EXPECT_DOUBLE_EQ(iGPU->utilizationPercent, 20.0) << "Not the NPU's 35%";

    const GPUCounters* npu = findCounters(counters, "PCI_00:0B.0_8086:7D1D");
    ASSERT_NE(npu, nullptr);
    EXPECT_EQ(npu->kind, DeviceKind::Npu);
    EXPECT_TRUE(npu->utilizationAvailable);
    EXPECT_DOUBLE_EQ(npu->utilizationPercent, 35.0);
    EXPECT_TRUE(npu->memoryAvailable);
    EXPECT_EQ(npu->memoryUsedBytes, 8192U) << "Its shared segment's usage";
    EXPECT_EQ(npu->memoryTotalBytes, 16 * GIB) << "The shared system memory it may map";

    const GPUCounters* accelerator = findCounters(counters, "PCI_03:00.0_1234:5678");
    ASSERT_NE(accelerator, nullptr);
    EXPECT_EQ(accelerator->kind, DeviceKind::ComputeAccelerator);
    EXPECT_EQ(accelerator->memoryTotalBytes, 8 * GIB);
    EXPECT_TRUE(accelerator->utilizationAvailable) << "A successful collect with no engine of it running: idle";
    EXPECT_DOUBLE_EQ(accelerator->utilizationPercent, 0.0);
}

// An enumerated NPU's per-process samples are no longer kept apart (#1826) but returned, tagged as
// the NPU's, so Domain counts them as NPU usage; the GPU's stay GPU samples, and the Basic Render
// Driver's and the absent NVIDIA GPU's are still kept apart.
TEST_F(WindowsGPUAcceleratorTest, ProcessSamplesOnAnEnumeratedNpuAreTaggedWithItsKind)
{
    setAdapters({intelIGPU(), basicRenderDriver()});
    dxgiState().accelerators = {intelNPU()};
    WindowsGPUProbe probe;
    useAcceleratorFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    const auto counters = probe.readProcessGPUCounters();

    ASSERT_EQ(counters.size(), 3U);
    const ProcessGPUCounters* iGPU = findProcess(counters, 10, LUID_IGPU);
    ASSERT_NE(iGPU, nullptr);
    EXPECT_EQ(iGPU->kind, DeviceKind::Gpu);
    EXPECT_DOUBLE_EQ(iGPU->gpuUtilPercent, 20.0);
    const ProcessGPUCounters* npuAndGPU = findProcess(counters, 10, LUID_NPU);
    ASSERT_NE(npuAndGPU, nullptr);
    EXPECT_EQ(npuAndGPU->kind, DeviceKind::Npu);
    EXPECT_DOUBLE_EQ(npuAndGPU->gpuUtilPercent, 80.0);
    EXPECT_EQ(npuAndGPU->gpuMemoryBytes, 8192U);
    EXPECT_EQ(npuAndGPU->activeEngines, std::vector<std::string>{"Compute"});
    const ProcessGPUCounters* npuOnly = findProcess(counters, 11, LUID_NPU);
    ASSERT_NE(npuOnly, nullptr);
    EXPECT_EQ(npuOnly->kind, DeviceKind::Npu);
    EXPECT_DOUBLE_EQ(npuOnly->gpuUtilPercent, 60.0);

    const auto other = probe.otherAdapterUsage();
    ASSERT_EQ(other.size(), 2U) << "The Basic Render Driver's and the absent NVIDIA GPU's";
    for (const auto& usage : other)
    {
        EXPECT_NE(usage.adapterLuid, pdhLuid(LUID_NPU));
    }
}

// The composite advertises per-process NPU data and utilization from its per-process PDH probe, as
// it does the GPU's (#1973) -- without the per-process PDH probe, neither (DXGI and NVML never report
// an NPU's per-process figures).
TEST_F(WindowsGPUAcceleratorTest, PerProcessNpuCapabilitiesComeFromThePerProcessPDHProbe)
{
    setAdapters({intelIGPU()});
    dxgiState().accelerators = {intelNPU()};
    WindowsGPUProbe probe;
    useAcceleratorFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    const GPUCapabilities caps = probe.capabilities();
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.hasPerProcessNpuMetrics);
    EXPECT_TRUE(caps.hasPerProcessNpuUtilization);
    EXPECT_TRUE(perProcessNpuMetricsSupported(caps));
    EXPECT_TRUE(perProcessNpuUtilizationSupported(caps));

    WindowsGPUProbeTestAccessor::dropPDH(probe);
    const GPUCapabilities withoutPDH = probe.capabilities();
    EXPECT_FALSE(withoutPDH.hasPerProcessMetrics);
    EXPECT_FALSE(withoutPDH.hasPerProcessNpuMetrics);
    EXPECT_FALSE(withoutPDH.hasPerProcessNpuUtilization);
    EXPECT_FALSE(perProcessNpuMetricsSupported(withoutPDH));
}

// The accelerator set is compared at every full rescan: one removed, or back under a new LUID after
// a driver reset, asks for a re-enumeration -- DXGI's factory stays current, as DXGI never lists them
// -- and the NPU keeps its id, so its history carries on. A quick rescan doesn't look.
TEST_F(WindowsGPUAcceleratorTest, AFullRescanPicksUpAChangedAccelerator)
{
    setAdapters({intelIGPU()});
    dxgiState().accelerators = {intelNPU()};
    WindowsGPUProbe probe;
    useAcceleratorFakes(probe);
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "Nothing changed";

    dxgiState().accelerators.clear();
    const int listingsBefore = dxgiState().acceleratorListings;
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(dxgiState().acceleratorListings, listingsBefore) << "A quick rescan doesn't list DXCore's adapters";
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full)) << "The NPU was removed";
    const auto withoutNPU = probe.enumerateGPUs();
    ASSERT_EQ(withoutNPU.size(), 1U);
    EXPECT_EQ(withoutNPU[0].kind, DeviceKind::Gpu);

    dxgiState().accelerators = {intelNPU(0x19000)};
    ASSERT_TRUE(probe.rescanGPUs(GPURescan::Full)) << "Back under a new LUID";
    const auto withNPU = probe.enumerateGPUs();
    const GPUInfo* npu = findById(withNPU, "PCI_00:0B.0_8086:7D1D");
    ASSERT_NE(npu, nullptr) << "The same id as before the reset";
    EXPECT_EQ(npu->luidId, pdhLuid(0x19000));
    EXPECT_EQ(npu->kind, DeviceKind::Npu);
}

} // namespace
} // namespace Platform

#endif // _WIN32
