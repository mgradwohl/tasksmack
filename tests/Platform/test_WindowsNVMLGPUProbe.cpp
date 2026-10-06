#ifdef _WIN32

#include "Mocks/WindowsNVMLFake.h"
#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"
#include "Platform/Windows/NVMLGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
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
// Basic Smoke Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, ConstructionDoesNotThrow)
{
    // Should not throw even if NVML is not available
    EXPECT_NO_THROW(NVMLGPUProbe probe);
}

TEST(WindowsNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

// ==========================================================================
// Availability Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, IsAvailableReturnsBool)
{
    NVMLGPUProbe probe;
    const bool available1 = probe.isAvailable();
    const bool available2 = probe.isAvailable();
    EXPECT_EQ(available1, available2) << "isAvailable() should be stable across consecutive calls";
}

TEST(WindowsNVMLGPUProbeTest, AvailabilityMatchesCapabilities)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto caps = probe.capabilities();

    EXPECT_EQ(caps.hasTemperature, available);
    EXPECT_EQ(caps.hasPowerMetrics, available);
    EXPECT_EQ(caps.hasClockSpeeds, available);
    EXPECT_EQ(caps.hasFanSpeed, available);
    EXPECT_EQ(caps.supportsMultiGPU, available);
}

// ==========================================================================
// Empty Enumeration Tests (when probe is unavailable)
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, EnumerationBehaviorMatchesAvailability)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    auto gpus = probe.enumerateGPUs();

    if (!available)
    {
        EXPECT_TRUE(gpus.empty()) << "Unavailable NVML should return empty GPU list";
    }
    else
    {
        for (const auto& gpu : gpus)
        {
            EXPECT_FALSE(gpu.id.empty()) << "GPU id should not be empty when NVML is available";
            EXPECT_FALSE(gpu.name.empty()) << "GPU name should not be empty when NVML is available";
        }
    }
}

TEST(WindowsNVMLGPUProbeTest, CounterDataIsWellFormedWhenPresent)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto gpus = probe.enumerateGPUs();
    auto counters = probe.readGPUCounters();

    if (available)
    {
        EXPECT_FALSE(gpus.empty()) << "Available NVML should enumerate at least one GPU before reading counters";
    }

    for (const auto& counter : counters)
    {
        EXPECT_FALSE(counter.gpuId.empty()) << "GPU counter id should not be empty";
        EXPECT_GE(counter.utilizationPercent, 0.0);
        EXPECT_LE(counter.utilizationPercent, 100.0);
    }
}

TEST(WindowsNVMLGPUProbeTest, ProcessCounterDataIsWellFormedWhenPresent)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto gpus = probe.enumerateGPUs();
    auto counters = probe.readProcessGPUCounters();

    if (available)
    {
        EXPECT_FALSE(gpus.empty()) << "Available NVML should enumerate at least one GPU before reading process counters";
    }

    for (const auto& counter : counters)
    {
        EXPECT_GE(counter.pid, 0) << "Process id should be non-negative";
    }
}

// ==========================================================================
// Capabilities Tests
// ==========================================================================

TEST(WindowsNVMLGPUProbeTest, CapabilitiesMatchAvailability)
{
    NVMLGPUProbe probe;
    const bool available = probe.isAvailable();
    const auto caps = probe.capabilities();

    if (!available)
    {
        EXPECT_FALSE(caps.hasTemperature);
        EXPECT_FALSE(caps.hasPowerMetrics);
        EXPECT_FALSE(caps.hasClockSpeeds);
        EXPECT_FALSE(caps.hasFanSpeed);
        EXPECT_FALSE(caps.hasPCIeMetrics);
        EXPECT_FALSE(caps.hasPerProcessMetrics);
        EXPECT_FALSE(caps.supportsMultiGPU);
    }
    else
    {
        EXPECT_TRUE(caps.hasTemperature);
        EXPECT_TRUE(caps.hasPowerMetrics);
        EXPECT_TRUE(caps.hasClockSpeeds);
        EXPECT_TRUE(caps.hasFanSpeed);
        // NVML only exposes PCIe throughput as rates, not the cumulative counters
        // GPUTypes.h expects, so this probe deliberately reports the capability as false.
        EXPECT_FALSE(caps.hasPCIeMetrics);
        EXPECT_TRUE(caps.supportsMultiGPU);
    }
}

TEST(WindowsNVMLGPUProbeTest, AvailableProbeEnumerationIsStable)
{
    NVMLGPUProbe probe;
    if (!probe.isAvailable())
    {
        GTEST_SKIP() << "NVML not available (no NVIDIA GPU or driver detected)";
    }

    auto gpus1 = probe.enumerateGPUs();
    auto gpus2 = probe.enumerateGPUs();

    EXPECT_EQ(gpus1.size(), gpus2.size()) << "Available NVML enumeration should be stable across calls";

    for (std::size_t i = 0; i < gpus1.size(); ++i)
    {
        EXPECT_FALSE(gpus1[i].id.empty()) << "GPU id should not be empty when available";
        EXPECT_FALSE(gpus1[i].name.empty()) << "GPU name should not be empty when available";
        EXPECT_EQ(gpus1[i].id, gpus2[i].id) << "GPU id should be stable across enumerations";
        EXPECT_EQ(gpus1[i].name, gpus2[i].name) << "GPU name should be stable across enumerations";
    }
}

} // namespace

// ==========================================================================
// Fake-NVML-backed tests
//
// loadNVML() deliberately restricts nvml.dll's search path to LOAD_LIBRARY_SEARCH_SYSTEM32
// (security hardening so a portable installation cannot load an adjacent DLL), so - unlike
// Linux's dlopen-based NVML probe - a fake DLL placed elsewhere cannot be picked up. Instead,
// NVMLGPUProbeTestAccessor (a friend of NVMLGPUProbe, see NVMLGPUProbe.h) lets tests substitute
// a fake NVMLFunctions table and device handles after construction (the constructor's real
// loadNVML() still runs as normal first; see NVMLGPUProbeTestAccessor::inject() in
// Mocks/WindowsNVMLFake.h for how
// any real backend it loaded is torn down first). This exercises enumerateGPUs()/
// readGPUCounters()/readProcessGPUCounters()/capabilities() deterministically without a real
// NVIDIA GPU, without weakening the production DLL-loading path in any way.
// ==========================================================================

namespace
{

using namespace Platform::NVML;     // NOLINT(google-build-using-namespace) - test fakes mirror the C API
using namespace Platform::NVMLFake; // NOLINT(google-build-using-namespace)

// ---- Fixture -------------------------------------------------------------

class NVMLGPUProbeFakeTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        fakeState() = FakeNvmlState{};
    }
};

// ==========================================================================
// enumerateGPUs
// ==========================================================================

TEST_F(NVMLGPUProbeFakeTest, DeviceCountFailureReturnsEmpty)
{
    fakeState().deviceCountResult = NVML_ERROR_UNKNOWN;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    EXPECT_TRUE(probe.enumerateGPUs().empty());
}

TEST_F(NVMLGPUProbeFakeTest, EnumerateSkipsDeviceWithFailedHandle)
{
    fakeState().deviceCount = 2;
    fakeState().invalidHandleIndices.insert(0);
    deviceData(1).name = "NVIDIA GeForce RTX 4080";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].name, "NVIDIA GeForce RTX 4080");
    EXPECT_EQ(gpus[0].deviceIndex, 1U);
    EXPECT_EQ(gpus[0].vendor, "NVIDIA");
    EXPECT_FALSE(gpus[0].isIntegrated);
}

TEST_F(NVMLGPUProbeFakeTest, EnumerateUsesUuidAsIdWhenAvailable)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-abc123";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "GPU-abc123");
}

TEST_F(NVMLGPUProbeFakeTest, EnumerateFallsBackToIndexBasedIdWhenUuidFails)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuidOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "NVML_GPU0");
}

// #1091: the PCI identity NVML reports is what pairs each NVML device with its DXGI adapter, so each
// device must carry its own bus, device number and PCI device id.
TEST_F(NVMLGPUProbeFakeTest, EnumerateRecordsEachDevicesPciIdentity)
{
    fakeState().deviceCount = 2;
    deviceData(0).pciBus = 0x01;
    deviceData(0).pciDevice = 0x00;
    deviceData(0).pciDeviceId = 0x268410DEU;
    deviceData(1).pciBus = 0x41;
    deviceData(1).pciDevice = 0x03;
    deviceData(1).pciDeviceId = 0x270410DEU;
    deviceData(1).pciBusId = "00000000:41:03.1"; // The function number comes from busId

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    ASSERT_TRUE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciLocation.value_or(PciLocation{}), (PciLocation{.bus = 0x01, .device = 0x00, .function = std::nullopt}))
        << "No busId: function unknown";
    EXPECT_EQ(gpus[0].pciDeviceId, 0x268410DEU);
    ASSERT_TRUE(gpus[1].pciLocation.has_value());
    EXPECT_EQ(gpus[1].pciLocation.value_or(PciLocation{}), (PciLocation{.bus = 0x41, .device = 0x03, .function = 1}));
    EXPECT_EQ(gpus[1].pciDeviceId, 0x270410DEU);
}

// A failed PCI query leaves that device's identity unknown -- no location and a zero id -- rather than
// a default location such as bus 0, which could match another adapter.
TEST_F(NVMLGPUProbeFakeTest, EnumerateLeavesPciIdentityUnknownWhenTheQueryFails)
{
    fakeState().deviceCount = 2;
    deviceData(0).pciInfoOk = false;
    deviceData(1).pciBus = 0x41;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_FALSE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciDeviceId, 0U);
    ASSERT_TRUE(gpus[1].pciLocation.has_value());
    EXPECT_EQ(gpus[1].pciLocation.value_or(PciLocation{}).bus, 0x41U);
}

// Drivers without nvmlDeviceGetPciInfo_v3/_v2 enumerate as before, with no PCI identity.
TEST_F(NVMLGPUProbeFakeTest, EnumerateLeavesPciIdentityUnknownWithoutThePciFunction)
{
    fakeState().deviceCount = 1;
    deviceData(0).pciBus = 0x41; // Would be reported, were the function there

    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetPciInfo = nullptr;
    NVMLGPUProbeTestAccessor::inject(probe, fns, /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_FALSE(gpus[0].pciLocation.has_value());
    EXPECT_EQ(gpus[0].pciDeviceId, 0U);
}

TEST_F(NVMLGPUProbeFakeTest, EnumerateLeavesDriverVersionUnknownWhenVbiosFails)
{
    fakeState().deviceCount = 1;
    deviceData(0).vbiosOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_TRUE(gpus[0].driverVersion.empty());
}

TEST_F(NVMLGPUProbeFakeTest, EnumerateRecordsWhichSensorsEachDeviceReports)
{
    // Sensor capabilities are per device (#1040): of two cards, one reports no fan or power and
    // the other no temperature or clock.
    fakeState().deviceCount = 2;
    deviceData(0).fanOk = false;
    deviceData(0).powerOk = false;
    deviceData(1).temperatureOk = false;
    deviceData(1).gpuClockOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    const auto first = gpus[0].sensorCapabilities.value_or(Platform::GPUCapabilities{});
    const auto second = gpus[1].sensorCapabilities.value_or(Platform::GPUCapabilities{});
    EXPECT_TRUE(gpus[0].sensorCapabilities.has_value());
    EXPECT_TRUE(first.hasTemperature);
    EXPECT_FALSE(first.hasPowerMetrics);
    EXPECT_TRUE(first.hasClockSpeeds);
    EXPECT_FALSE(first.hasFanSpeed);
    EXPECT_TRUE(gpus[1].sensorCapabilities.has_value());
    EXPECT_FALSE(second.hasTemperature);
    EXPECT_TRUE(second.hasPowerMetrics);
    EXPECT_FALSE(second.hasClockSpeeds);
    EXPECT_TRUE(second.hasFanSpeed);
}

TEST_F(NVMLGPUProbeFakeTest, CountersKeepTheIdEnumerationReported)
{
    // A UUID read that succeeds at enumeration but fails later must not change the device's id,
    // or the DXGI merge would lose its NVML metrics (#1040).
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-abc123";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    deviceData(0).uuidOk = false;

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuId, "GPU-abc123");
}

// #1265: a sleeping GPU (a hybrid laptop's runtime-suspended dGPU) is left alone: a repeat
// enumeration makes no call addressed to it -- its identity and sensor set were read while it was
// awake, and are kept (#1294) -- and neither do counter and process reads. Its counters say it is
// suspended, with every reading unavailable and the VRAM total from the last read while it was
// awake; the awake GPU is read as usual.
TEST_F(NVMLGPUProbeFakeTest, ASleepingGpuIsNotQueried)
{
    fakeState().deviceCount = 2;
    deviceData(0).pciBus = 0x01;
    deviceData(0).memTotal = 8ULL << 30U;
    deviceData(1).pciBus = 0x41;
    deviceData(1).uuid = "GPU-22222222-2222-2222-2222-222222222222";
    fakeState().graphicsProcesses[0] = makeProcessQuery({{.pid = 1234, .usedGpuMemory = 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    bool firstAsleep = false;
    NVMLGPUProbeTestAccessor::setAsleep(probe, [&firstAsleep](const PciLocation& location) { return firstAsleep && location.bus == 0x01; });

    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    const auto awake = probe.readGPUCounters(); // Awake: its VRAM total is read
    ASSERT_EQ(awake.size(), 2U);
    EXPECT_FALSE(awake[0].suspended);
    EXPECT_FALSE(awake[1].suspended);

    firstAsleep = true;
    fakeState().deviceQueries.clear();
    const auto gpus = probe.enumerateGPUs();
    const auto asleep = probe.readGPUCounters();
    const auto processes = probe.readProcessGPUCounters();

    // The repeat enumeration reads nothing from it, and keeps the sensor set found while it was awake.
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(fakeState().deviceQueries[0], 0);
    EXPECT_TRUE(gpus[0].sensorCapabilities.has_value());
    EXPECT_TRUE(gpus[1].sensorCapabilities.has_value());

    ASSERT_EQ(asleep.size(), 2U);
    const auto sleeping = std::ranges::find(asleep, std::string("GPU-11111111-1111-1111-1111-111111111111"), &GPUCounters::gpuId);
    const auto other = std::ranges::find(asleep, std::string("GPU-22222222-2222-2222-2222-222222222222"), &GPUCounters::gpuId);
    ASSERT_NE(sleeping, asleep.end());
    ASSERT_NE(other, asleep.end());
    EXPECT_TRUE(sleeping->suspended);
    EXPECT_FALSE(sleeping->utilizationAvailable);
    EXPECT_FALSE(sleeping->temperatureAvailable);
    EXPECT_FALSE(sleeping->powerAvailable);
    EXPECT_FALSE(sleeping->gpuClockAvailable);
    EXPECT_FALSE(sleeping->memoryAvailable);
    EXPECT_EQ(sleeping->memoryTotalBytes, 8ULL << 30U);
    EXPECT_FALSE(other->suspended);
    EXPECT_TRUE(other->temperatureAvailable);

    EXPECT_TRUE(processes.empty()); // Its process (pid 1234) isn't listed: that would ask the GPU
    EXPECT_EQ(fakeState().deviceQueries[0], 0) << "No NVML call reached the sleeping GPU";
    EXPECT_GT(fakeState().deviceQueries[1], 3);
}

// ==========================================================================
// rescanGPUs (#1294)
// ==========================================================================

// A GPU asleep at its first enumeration has only its identity read (name, UUID, PCI) and no sensor
// set. A quick rescan reports nothing while it sleeps, then a change once it is awake -- asking only
// the power state -- and the re-enumeration that follows finds its own sensors.
TEST_F(NVMLGPUProbeFakeTest, AGpuAsleepAtEnumerationGetsItsSensorsOnceAwake)
{
    fakeState().deviceCount = 1;
    deviceData(0).fanOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    bool asleep = true;
    NVMLGPUProbeTestAccessor::setAsleep(probe, [&asleep](const PciLocation&) { return asleep; });

    const auto sleeping = probe.enumerateGPUs();
    ASSERT_EQ(sleeping.size(), 1U);
    EXPECT_FALSE(sleeping[0].sensorCapabilities.has_value());
    EXPECT_EQ(fakeState().deviceQueries[0], 3) << "Only its name, UUID and PCI identity are read";

    fakeState().deviceQueries.clear();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));

    asleep = false;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(fakeState().deviceQueries[0], 0) << "A rescan asks the power state, not the GPU";

    const auto awake = probe.enumerateGPUs();
    ASSERT_EQ(awake.size(), 1U);
    ASSERT_TRUE(awake[0].sensorCapabilities.has_value());
    EXPECT_TRUE(awake[0].sensorCapabilities.value_or(GPUCapabilities{}).hasTemperature);
    EXPECT_FALSE(awake[0].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    EXPECT_EQ(awake[0].id, sleeping[0].id);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "Found once: nothing more to report";
}

// A reading that reports the GPU lost (a driver reset) re-initialises NVML at the next full rescan,
// not the next sample, and reports the change; the re-enumeration lists the GPUs present now, by
// their new indices, and a returning GPU keeps its id and the sensor set found before.
TEST_F(NVMLGPUProbeFakeTest, AGpuLostErrorReinitialisesNVMLAtTheNextFullRescan)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";
    deviceData(0).pciBus = 0x01;
    deviceData(0).fanOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    ASSERT_EQ(probe.readGPUCounters().size(), 1U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "Nothing lost yet";

    fakeState().lostDevices.insert(0);
    const auto lost = probe.readGPUCounters();
    ASSERT_EQ(lost.size(), 1U);
    EXPECT_FALSE(lost[0].memoryAvailable);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "A quick rescan doesn't re-initialise";
    EXPECT_EQ(fakeState().initCallCount, 0);

    // After the reset the GPU is back as index 1, behind a newly listed one; the fan still can't be read.
    fakeState().lostDevices.clear();
    fakeState().deviceCount = 2;
    fakeState().devices[1] = fakeState().devices[0];
    fakeState().devices[0] = FakeDeviceData{};
    deviceData(0).uuid = "GPU-bbbb";
    deviceData(0).pciBus = 0x41;
    const int shutdownsBefore = fakeState().shutdownCallCount;
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 1);
    EXPECT_EQ(fakeState().shutdownCallCount, shutdownsBefore + 1);
    EXPECT_TRUE(probe.isAvailable());

    fakeState().deviceQueries.clear();
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(fakeState().deviceQueries[1], 3) << "The returning GPU's identity is read; its sensors are already known";
    EXPECT_GT(fakeState().deviceQueries[0], 3) << "The new GPU's sensors are probed";
    EXPECT_EQ(gpus[0].id, "GPU-bbbb");
    EXPECT_EQ(gpus[1].id, "GPU-aaaa");
    EXPECT_EQ(gpus[1].pciLocation.value_or(PciLocation{}).bus, 0x01U);
    EXPECT_FALSE(gpus[1].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_TRUE(std::ranges::all_of(counters, [](const GPUCounters& counter) { return counter.memoryAvailable; }));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << "Lost is cleared by the restart";
}

// A GPU WindowsGPUProbe marks idle (PDH saw no activity on it) gets no NVML call, which could keep a
// hybrid dGPU from suspending (#1265): its previous readings and process list are repeated, not
// zeroed or marked unread. Once it is no longer idle it is read again.
TEST_F(NVMLGPUProbeFakeTest, AnIdleGPUIsNotQueriedAndKeepsItsPreviousReadings)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";
    deviceData(0).temperatureC = 45;
    fakeState().graphicsProcesses[0] = makeProcessQuery({{.pid = 1234, .usedGpuMemory = 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    ASSERT_EQ(probe.readGPUCounters().size(), 1U);
    ASSERT_EQ(probe.readProcessGPUCounters().size(), 1U);

    probe.setIdleDevices({"GPU-aaaa"});
    deviceData(0).temperatureC = 60;
    fakeState().graphicsProcesses[0] = makeProcessQuery({});
    fakeState().deviceQueries.clear();
    const auto idle = probe.readGPUCounters();
    const auto idleProcesses = probe.readProcessGPUCounters();
    EXPECT_EQ(fakeState().deviceQueries[0], 0) << "No NVML call reached the idle GPU";
    ASSERT_EQ(idle.size(), 1U);
    EXPECT_FALSE(idle[0].suspended) << "Idle is not asleep";
    EXPECT_TRUE(idle[0].temperatureAvailable);
    EXPECT_EQ(idle[0].temperatureC, 45) << "The previous reading stands";
    EXPECT_EQ(idle[0].gpuId, "GPU-aaaa");
    ASSERT_EQ(idleProcesses.size(), 1U);
    EXPECT_EQ(idleProcesses[0].pid, 1234);

    probe.setIdleDevices({});
    const auto busy = probe.readGPUCounters();
    ASSERT_EQ(busy.size(), 1U);
    EXPECT_EQ(busy[0].temperatureC, 60);
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

// A GPU marked idle before NVML has ever read it is read anyway: there is nothing to repeat.
TEST_F(NVMLGPUProbeFakeTest, AnIdleGPUWithNoPreviousReadingIsRead)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    probe.setIdleDevices({"GPU-aaaa"});
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_TRUE(counters[0].temperatureAvailable);
    EXPECT_EQ(counters[0].temperatureC, 63);
}

// A GPU lost while enumeration probes its sensors (a driver reset as it wakes) tells nothing about
// which sensors it has: the set stays unknown rather than cached as "none", the loss restarts NVML
// at the next full rescan, and the enumeration after that finds the real set. Caching the failed
// probe left the GPU without sensors for good, since the set survives restart().
TEST_F(NVMLGPUProbeFakeTest, AGpuLostDuringTheSensorProbeLeavesTheSensorsUnknownUntilNVMLRestarts)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";
    fakeState().lostDevices.insert(0);

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    const auto lost = probe.enumerateGPUs();
    ASSERT_EQ(lost.size(), 1U);
    EXPECT_FALSE(lost[0].sensorCapabilities.has_value()) << "A failed probe is not an empty sensor set";
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)) << "No re-enumeration until NVML has restarted";
    EXPECT_EQ(fakeState().initCallCount, 0);

    fakeState().lostDevices.clear();
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)) << "The loss the probe saw restarts NVML";
    EXPECT_EQ(fakeState().initCallCount, 1);
    const auto back = probe.enumerateGPUs();
    ASSERT_EQ(back.size(), 1U);
    ASSERT_TRUE(back[0].sensorCapabilities.has_value());
    EXPECT_TRUE(back[0].sensorCapabilities->hasTemperature);
    EXPECT_TRUE(back[0].sensorCapabilities->hasFanSpeed);
}

// NVML_ERROR_UNINITIALIZED from a running-process query is a reset too: NVML restarts at the next
// full rescan, as it does after a counter read's.
TEST_F(NVMLGPUProbeFakeTest, AResetReportedByAProcessQueryRestartsNVMLAtTheNextFullRescan)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    fakeState().graphicsProcesses[0] = makeProcessQuery({}, NVML_ERROR_UNINITIALIZED);
    static_cast<void>(probe.readProcessGPUCounters());

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 1);
}

// NVML_ERROR_UNINITIALIZED means the same as a lost GPU: the library has to be started again. A
// re-init that fails reports no change (the GPUs are still there; their readings are gaps) and
// leaves NVML unavailable, ready to be retried.
TEST_F(NVMLGPUProbeFakeTest, AFailedReinitialisationReportsNoChange)
{
    fakeState().deviceCount = 1;
    deviceData(0).uuid = "GPU-aaaa";

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    fakeState().deviceCountResult = NVML_ERROR_UNINITIALIZED;
    EXPECT_TRUE(probe.enumerateGPUs().empty());

    fakeState().initResult = NVML_ERROR_DRIVER_NOT_LOADED;
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(fakeState().initCallCount, 1);
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.isLoaded());

    fakeState().deviceCountResult = NVML_SUCCESS;
    fakeState().initResult = NVML_SUCCESS;
    EXPECT_TRUE(probe.restart());
    EXPECT_EQ(probe.enumerateGPUs().size(), 1U);
}

// ==========================================================================
// readGPUCounters
// ==========================================================================

TEST_F(NVMLGPUProbeFakeTest, ReadGPUCountersPopulatesAllFieldsOnSuccess)
{
    auto& d = deviceData(0);
    d.memUsed = 4ULL * 1024 * 1024 * 1024;
    d.memTotal = 16ULL * 1024 * 1024 * 1024;
    d.temperatureC = 71;
    d.powerMilliwatts = 220000;
    d.powerLimitMilliwatts = 320000;
    d.gpuClockMhz = 1980;
    d.memClockMhz = 9500;
    d.utilizationGpu = 55;
    d.fanPercent = 60;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    const auto& c = counters[0];
    EXPECT_EQ(c.memoryUsedBytes, d.memUsed);
    EXPECT_EQ(c.memoryTotalBytes, d.memTotal);
    EXPECT_EQ(c.temperatureC, 71);
    EXPECT_DOUBLE_EQ(c.powerDrawWatts, 220.0);
    EXPECT_DOUBLE_EQ(c.powerLimitWatts, 320.0);
    EXPECT_EQ(c.gpuClockMHz, 1980U);
    EXPECT_EQ(c.memoryClockMHz, 9500U);
    EXPECT_DOUBLE_EQ(c.utilizationPercent, 55.0);
    EXPECT_EQ(c.fanSpeedRaw, 60U);
    EXPECT_EQ(c.fanSpeedMaxRaw, 100U);
    EXPECT_TRUE(c.temperatureAvailable);
    EXPECT_TRUE(c.powerAvailable);
    EXPECT_TRUE(c.utilizationAvailable);
    EXPECT_TRUE(c.memoryAvailable);
    EXPECT_TRUE(c.gpuClockAvailable);
}

TEST_F(NVMLGPUProbeFakeTest, ReadGPUCountersLeavesFieldsAtDefaultOnPerMetricFailure)
{
    auto& d = deviceData(0);
    d.temperatureOk = false;
    d.powerOk = false;
    d.utilizationOk = false;
    d.fanOk = false;
    d.memoryOk = false;
    d.gpuClockOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    const auto& c = counters[0];
    EXPECT_EQ(c.temperatureC, 0);
    EXPECT_DOUBLE_EQ(c.powerDrawWatts, 0.0);
    EXPECT_DOUBLE_EQ(c.utilizationPercent, 0.0);
    EXPECT_EQ(c.fanSpeedRaw, 0U);
    // Each failed read is marked unread, so it publishes as a gap rather than a real 0 (#1111).
    EXPECT_FALSE(c.temperatureAvailable);
    EXPECT_FALSE(c.powerAvailable);
    EXPECT_FALSE(c.utilizationAvailable);
    EXPECT_FALSE(c.memoryAvailable);
    EXPECT_FALSE(c.gpuClockAvailable);
}

TEST_F(NVMLGPUProbeFakeTest, ReadGPUCountersFallsBackToIndexIdWhenUuidFails)
{
    deviceData(0).uuidOk = false;

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuId, "NVML_GPU0");
}

// ==========================================================================
// readProcessGPUCounters
// ==========================================================================

TEST_F(NVMLGPUProbeFakeTest, ProcessCountersEmptyWhenNoPerProcessFunctionsAvailable)
{
    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetComputeRunningProcesses = {};
    fns.DeviceGetGraphicsRunningProcesses = {};
    NVMLGPUProbeTestAccessor::inject(probe, fns, /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST_F(NVMLGPUProbeFakeTest, ComputeOnlyProcessIsReportedWithComputeEngine)
{
    fakeState().computeProcesses[0] =
        makeProcessQuery({{.pid = 1234, .usedGpuMemory = 512ULL * 1024 * 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 1234);
    EXPECT_EQ(counters[0].gpuMemoryBytes, 512U * 1024 * 1024);
    ASSERT_EQ(counters[0].activeEngines.size(), 1U);
    EXPECT_EQ(counters[0].activeEngines[0], "Compute");
}

TEST_F(NVMLGPUProbeFakeTest, GraphicsOnlyProcessIsReportedWithGraphicsEngine)
{
    fakeState().graphicsProcesses[0] =
        makeProcessQuery({{.pid = 5678, .usedGpuMemory = 256ULL * 1024 * 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 5678);
    ASSERT_EQ(counters[0].activeEngines.size(), 1U);
    EXPECT_EQ(counters[0].activeEngines[0], "3D");
}

TEST_F(NVMLGPUProbeFakeTest, SamePidInBothListsMergesEnginesAndKeepsMaxMemory)
{
    constexpr unsigned int pid = 42;
    fakeState().computeProcesses[0] =
        makeProcessQuery({{.pid = pid, .usedGpuMemory = 100ULL * 1024 * 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});
    fakeState().graphicsProcesses[0] =
        makeProcessQuery({{.pid = pid, .usedGpuMemory = 300ULL * 1024 * 1024, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U) << "Same PID on the same GPU should merge into one entry";
    EXPECT_EQ(counters[0].gpuMemoryBytes, 300U * 1024 * 1024) << "Merge should keep the larger valid memory value";
    ASSERT_EQ(counters[0].activeEngines.size(), 2U);
    EXPECT_EQ(counters[0].activeEngines[0], "Compute");
    EXPECT_EQ(counters[0].activeEngines[1], "3D");
}

TEST_F(NVMLGPUProbeFakeTest, UnavailableMemorySentinelReportsZeroBytes)
{
    constexpr auto notAvailable = std::numeric_limits<unsigned long long>::max();
    fakeState().computeProcesses[0] =
        makeProcessQuery({{.pid = 7, .usedGpuMemory = notAvailable, .gpuInstanceId = 0, .computeInstanceId = 0}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuMemoryBytes, 0U);
}

TEST_F(NVMLGPUProbeFakeTest, ImplausibleReportedCountIsSkipped)
{
    fakeState().computeProcesses[0] = makeProcessQuery({}, NVML_SUCCESS, NVML_SUCCESS, 100000U);

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST_F(NVMLGPUProbeFakeTest, ImplausibleGraphicsReportedCountIsSkipped)
{
    // Mirrors ImplausibleReportedCountIsSkipped above, but for the graphics-process guard
    // rather than the compute-process one - they're separate branches in production code.
    fakeState().graphicsProcesses[0] = makeProcessQuery({}, NVML_SUCCESS, NVML_SUCCESS, 100000U);

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

TEST_F(NVMLGPUProbeFakeTest, InsufficientSizeOnFirstCallStillFetchesProcesses)
{
    fakeState().computeProcesses[0] =
        makeProcessQuery({{.pid = 99, .usedGpuMemory = 1024, .gpuInstanceId = 0, .computeInstanceId = 0}}, NVML_ERROR_INSUFFICIENT_SIZE);

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].pid, 99);
}

TEST_F(NVMLGPUProbeFakeTest, UnexpectedFirstCallErrorYieldsNoProcesses)
{
    fakeState().computeProcesses[0] = makeProcessQuery({}, NVML_ERROR_UNKNOWN);

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

// Running-process entry points (#1313): the loader takes the newest variant nvml.dll exports, and
// the entries are read at that variant's size (16 bytes for the unversioned v1 export, 24 for _v3).

TEST_F(NVMLGPUProbeFakeTest, LegacyV1RunningProcessesReadsEverySixteenByteEntry)
{
    // Before #1313 the probe read the v1 export's 16-byte entries as 24-byte ones, so every entry
    // after the first came from the wrong offset (the second PID read as the first's memory).
    fakeState().computeProcesses[0] = makeProcessQuery({{.pid = 111, .usedGpuMemory = 1000}, {.pid = 222, .usedGpuMemory = 2000}});
    fakeState().graphicsProcesses[0] = makeProcessQuery({{.pid = 333, .usedGpuMemory = 3000}, {.pid = 444, .usedGpuMemory = 4000}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::loadRunningProcesses(probe, runningProcessExports(/*withV1=*/true, /*withV3=*/false));
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));
    EXPECT_EQ(NVMLGPUProbeTestAccessor::computeEntrySize(probe), FAKE_PROCESS_INFO_V1_SIZE);

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 4U);
    EXPECT_EQ(counters[0].pid, 111);
    EXPECT_EQ(counters[0].gpuMemoryBytes, 1000U);
    EXPECT_EQ(counters[1].pid, 222);
    EXPECT_EQ(counters[1].gpuMemoryBytes, 2000U);
    EXPECT_EQ(counters[2].pid, 333);
    EXPECT_EQ(counters[2].gpuMemoryBytes, 3000U);
    EXPECT_EQ(counters[3].pid, 444);
    EXPECT_EQ(counters[3].gpuMemoryBytes, 4000U);
    EXPECT_TRUE(probe.capabilities().hasPerProcessMetrics);
}

TEST_F(NVMLGPUProbeFakeTest, V3RunningProcessesIsPreferredAndReadsTwentyFourByteEntries)
{
    fakeState().computeProcesses[0] = makeProcessQuery({{.pid = 111, .usedGpuMemory = 1000, .gpuInstanceId = 1, .computeInstanceId = 2},
                                                        {.pid = 222, .usedGpuMemory = 2000, .gpuInstanceId = 3, .computeInstanceId = 4}});

    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::loadRunningProcesses(probe, runningProcessExports(/*withV1=*/true, /*withV3=*/true));
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));
    EXPECT_EQ(NVMLGPUProbeTestAccessor::computeEntrySize(probe), FAKE_PROCESS_INFO_V2_SIZE) << "the _v3 export is preferred over v1";

    const auto counters = probe.readProcessGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[0].pid, 111);
    EXPECT_EQ(counters[0].gpuMemoryBytes, 1000U);
    EXPECT_EQ(counters[1].pid, 222);
    EXPECT_EQ(counters[1].gpuMemoryBytes, 2000U);
}

TEST_F(NVMLGPUProbeFakeTest, NoRunningProcessesExportMeansNoPerProcessMetrics)
{
    NVMLGPUProbe probe;
    NVMLGPUProbeTestAccessor::inject(probe, NVMLGPUProbeTestAccessor::fullFakeFunctions(), /*initialized=*/true);
    NVMLGPUProbeTestAccessor::loadRunningProcesses(probe, runningProcessExports(/*withV1=*/false, /*withV3=*/false));
    NVMLGPUProbeTestAccessor::addDevice(probe, 0, deviceHandleFor(0));

    EXPECT_FALSE(probe.capabilities().hasPerProcessMetrics);
    EXPECT_TRUE(probe.readProcessGPUCounters().empty());
}

// ==========================================================================
// capabilities
// ==========================================================================

TEST_F(NVMLGPUProbeFakeTest, CapabilitiesReportsPerProcessMetricsWhenEitherFunctionAvailable)
{
    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetGraphicsRunningProcesses = {}; // only compute available
    NVMLGPUProbeTestAccessor::inject(probe, fns, /*initialized=*/true);

    EXPECT_TRUE(probe.capabilities().hasPerProcessMetrics);
}

TEST_F(NVMLGPUProbeFakeTest, CapabilitiesReportsNoPerProcessMetricsWhenNeitherAvailable)
{
    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetComputeRunningProcesses = {};
    fns.DeviceGetGraphicsRunningProcesses = {};
    NVMLGPUProbeTestAccessor::inject(probe, fns, /*initialized=*/true);

    EXPECT_FALSE(probe.capabilities().hasPerProcessMetrics);
}

// ==========================================================================
// getNVMLErrorString (via accessor)
// ==========================================================================

TEST(NVMLGPUProbeErrorStringTest, KnownCodesMapToDistinctNonEmptyStrings)
{
    const std::vector<nvmlReturn_t> codes = {
        NVML_SUCCESS,
        NVML_ERROR_UNINITIALIZED,
        NVML_ERROR_INVALID_ARGUMENT,
        NVML_ERROR_NOT_SUPPORTED,
        NVML_ERROR_NO_PERMISSION,
        NVML_ERROR_ALREADY_INITIALIZED,
        NVML_ERROR_NOT_FOUND,
        NVML_ERROR_INSUFFICIENT_SIZE,
        NVML_ERROR_INSUFFICIENT_POWER,
        NVML_ERROR_DRIVER_NOT_LOADED,
        NVML_ERROR_TIMEOUT,
        NVML_ERROR_IRQ_ISSUE,
        NVML_ERROR_LIBRARY_NOT_FOUND,
        NVML_ERROR_FUNCTION_NOT_FOUND,
        NVML_ERROR_CORRUPTED_INFOROM,
        NVML_ERROR_GPU_IS_LOST,
    };

    std::unordered_set<std::string> seen;
    for (const auto code : codes)
    {
        const auto message = NVMLGPUProbeTestAccessor::errorString(code);
        EXPECT_FALSE(message.empty());
        EXPECT_TRUE(seen.insert(message).second) << "Error strings should be distinct per code";
    }
}

TEST(NVMLGPUProbeErrorStringTest, UnknownCodeFallsBackToGenericMessage)
{
    // Deliberately out of range: exercises the "Unknown error (N)" default branch.
    const auto message =
        NVMLGPUProbeTestAccessor::errorString(static_cast<nvmlReturn_t>(12345)); // NOLINT(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_TRUE(message.contains("12345"));
}

} // namespace
} // namespace Platform

#endif // _WIN32
