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
#include "Platform/Windows/DXGIGPUProbe.h"
#include "Platform/Windows/NVMLGPUProbe.h"
#include "Platform/Windows/PDHGPUProbe.h" // IWYU pragma: keep - WindowsGPUProbeTestAccessor::dropPDH() deletes one
#include "Platform/Windows/WindowsGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace Platform
{

/// Test-only accessor (a friend of WindowsGPUProbe): reaches its sub-probes so a test can back them
/// with fakes, and drops PDH, whose real counters would describe this machine's adapters.
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
    return makeAdapter(L"Intel(R) UHD Graphics", VENDOR_INTEL, 0x9A49, 0x100, PciLocation{.bus = 0x00, .device = 0x02}, 128ULL << 20U);
}

FakeAdapter nvidiaGPU(std::uint32_t luidLowPart, std::uint32_t bus)
{
    return makeAdapter(L"NVIDIA GeForce RTX 4090", VENDOR_NVIDIA, DEVICE_RTX, luidLowPart, PciLocation{.bus = bus, .device = 0x00});
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

} // namespace
} // namespace Platform

#endif // _WIN32
