#ifdef _WIN32

#include "Platform/NVMLTypes.h"
#include "Platform/Windows/NVMLGPUProbe.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
// loadNVML() still runs as normal first; see NVMLGPUProbeTestAccessor::inject() below for how
// any real backend it loaded is torn down first). This exercises enumerateGPUs()/
// readGPUCounters()/readProcessGPUCounters()/capabilities() deterministically without a real
// NVIDIA GPU, without weakening the production DLL-loading path in any way.
// ==========================================================================

namespace
{

using namespace Platform::NVML;

// ---- Fake NVML backend --------------------------------------------------

struct FakeDeviceData
{
    // Value fields grouped before the bools below to avoid padding between them.
    std::uint64_t memUsed = 2ULL * 1024 * 1024 * 1024;
    std::uint64_t memTotal = 24ULL * 1024 * 1024 * 1024;
    std::string name = "NVIDIA GeForce RTX 4090";
    std::string uuid = "GPU-11111111-1111-1111-1111-111111111111";
    std::string vbios = "95.02.18.00.01";
    unsigned int temperatureC = 63;
    unsigned int powerMilliwatts = 180000;
    unsigned int powerLimitMilliwatts = 450000;
    unsigned int gpuClockMhz = 2100;
    unsigned int memClockMhz = 10500;
    unsigned int utilizationGpu = 37;
    unsigned int fanPercent = 48;
    bool nameOk = true;
    bool uuidOk = true;
    bool vbiosOk = true;
    bool memoryOk = true;
    bool temperatureOk = true;
    bool powerOk = true;
    bool powerLimitOk = true;
    bool gpuClockOk = true;
    bool memClockOk = true;
    bool utilizationOk = true;
    bool fanOk = true;
};

struct FakeProcessQuery
{
    std::vector<nvmlProcessInfo_t> processes;
    nvmlReturn_t firstCallResult = NVML_SUCCESS;
    nvmlReturn_t secondCallResult = NVML_SUCCESS;
    // When set, the "query count" call reports this instead of processes.size() - lets a
    // test simulate an implausible driver-reported count independent of the real list size.
    std::optional<unsigned int> reportedCountOverride;
};

struct FakeNvmlState
{
    nvmlReturn_t deviceCountResult = NVML_SUCCESS;
    unsigned int deviceCount = 0;
    std::unordered_set<unsigned int> invalidHandleIndices;
    std::unordered_map<unsigned int, FakeDeviceData> devices;
    std::unordered_map<unsigned int, FakeProcessQuery> computeProcesses;
    std::unordered_map<unsigned int, FakeProcessQuery> graphicsProcesses;
    int shutdownCallCount = 0;
};

FakeNvmlState& fakeState()
{
    static FakeNvmlState state;
    return state;
}

FakeDeviceData& deviceData(unsigned int index)
{
    return fakeState().devices[index];
}

unsigned int deviceIndexOf(nvmlDevice_t device)
{
    return static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(device) - 1);
}

nvmlDevice_t deviceHandleFor(unsigned int index)
{
    // nvmlDevice_t is an opaque handle; encoding the index as a small sentinel pointer (never
    // dereferenced) is the simplest way for the fakes below to recover which device a call is for.
    return reinterpret_cast<nvmlDevice_t>(static_cast<std::uintptr_t>(index) + 1); // NOLINT(performance-no-int-to-ptr)
}

FakeProcessQuery makeProcessQuery(std::vector<nvmlProcessInfo_t> processes,
                                  nvmlReturn_t firstCallResult = NVML_SUCCESS,
                                  nvmlReturn_t secondCallResult = NVML_SUCCESS,
                                  std::optional<unsigned int> reportedCountOverride = std::nullopt)
{
    FakeProcessQuery query;
    query.processes = std::move(processes);
    query.firstCallResult = firstCallResult;
    query.secondCallResult = secondCallResult;
    query.reportedCountOverride = reportedCountOverride;
    return query;
}

void copyToBuffer(char* dst, unsigned int size, const std::string& s)
{
    if (size == 0)
    {
        return;
    }
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(size - 1), s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

nvmlReturn_t fakeDeviceGetCount(unsigned int* count)
{
    *count = fakeState().deviceCount;
    return fakeState().deviceCountResult;
}

nvmlReturn_t fakeDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t* device)
{
    if (fakeState().invalidHandleIndices.contains(index))
    {
        return NVML_ERROR_NOT_FOUND;
    }
    *device = deviceHandleFor(index);
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetName(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.nameOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    copyToBuffer(buf, size, d.name);
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetUUID(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.uuidOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    copyToBuffer(buf, size, d.uuid);
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetVbiosVersion(nvmlDevice_t device, char* buf, unsigned int size)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.vbiosOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    copyToBuffer(buf, size, d.vbios);
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetMemoryInfo(nvmlDevice_t device, void* memInfoRaw)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.memoryOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    auto* memInfo = static_cast<nvmlMemory_t*>(memInfoRaw);
    memInfo->used = d.memUsed;
    memInfo->total = d.memTotal;
    memInfo->free = d.memTotal - d.memUsed;
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetTemperature(nvmlDevice_t device, int /*sensor*/, unsigned int* temp)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.temperatureOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *temp = d.temperatureC;
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetPowerUsage(nvmlDevice_t device, unsigned int* mw)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.powerOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *mw = d.powerMilliwatts;
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int* mw)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.powerLimitOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *mw = d.powerLimitMilliwatts;
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetClockInfo(nvmlDevice_t device, int clockType, unsigned int* mhz)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (clockType == static_cast<int>(NVML_CLOCK_GRAPHICS))
    {
        if (!d.gpuClockOk)
        {
            return NVML_ERROR_NOT_SUPPORTED;
        }
        *mhz = d.gpuClockMhz;
        return NVML_SUCCESS;
    }
    if (clockType == static_cast<int>(NVML_CLOCK_MEM))
    {
        if (!d.memClockOk)
        {
            return NVML_ERROR_NOT_SUPPORTED;
        }
        *mhz = d.memClockMhz;
        return NVML_SUCCESS;
    }
    return NVML_ERROR_INVALID_ARGUMENT;
}

nvmlReturn_t fakeDeviceGetUtilizationRates(nvmlDevice_t device, void* utilRaw)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.utilizationOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    auto* util = static_cast<nvmlUtilization_t*>(utilRaw);
    util->gpu = d.utilizationGpu;
    util->memory = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t fakeDeviceGetFanSpeed(nvmlDevice_t device, unsigned int* speed)
{
    const auto& d = fakeState().devices.at(deviceIndexOf(device));
    if (!d.fanOk)
    {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *speed = d.fanPercent;
    return NVML_SUCCESS;
}

nvmlReturn_t
queryFakeProcesses(std::unordered_map<unsigned int, FakeProcessQuery>& table, unsigned int deviceIndex, unsigned int* count, void* buffer)
{
    auto it = table.find(deviceIndex);
    if (it == table.end())
    {
        *count = 0;
        return NVML_SUCCESS;
    }

    const auto& query = it->second;
    const unsigned int reportedCount = query.reportedCountOverride.value_or(static_cast<unsigned int>(query.processes.size()));

    if (buffer == nullptr)
    {
        *count = reportedCount;
        return query.firstCallResult;
    }

    auto* out = static_cast<nvmlProcessInfo_t*>(buffer);
    const unsigned int toCopy = std::min(*count, static_cast<unsigned int>(query.processes.size()));
    for (unsigned int i = 0; i < toCopy; ++i)
    {
        out[i] = query.processes[i];
    }
    *count = toCopy;
    return query.secondCallResult;
}

nvmlReturn_t fakeDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int* count, void* buffer)
{
    return queryFakeProcesses(fakeState().computeProcesses, deviceIndexOf(device), count, buffer);
}

nvmlReturn_t fakeDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int* count, void* buffer)
{
    return queryFakeProcesses(fakeState().graphicsProcesses, deviceIndexOf(device), count, buffer);
}

nvmlReturn_t fakeShutdown()
{
    ++fakeState().shutdownCallCount;
    return NVML_SUCCESS;
}

} // namespace

// Test-only accessor: lets unit tests inject a fake NVMLFunctions table and device handles
// so enumerateGPUs()/readGPUCounters()/readProcessGPUCounters()/capabilities() can be
// exercised deterministically without a real NVIDIA GPU or nvml.dll. Declared directly in
// namespace Platform (not inside an anonymous namespace) so the `friend struct
// NVMLGPUProbeTestAccessor;` declaration in NVMLGPUProbe.h resolves to this exact type; its
// members can still see the fakes above via the anonymous namespace's implicit visibility in
// the rest of this translation unit. inject() below substitutes the backend after the
// constructor's real loadNVML() has already run; this does not change or bypass
// loadNVML()'s LOAD_LIBRARY_SEARCH_SYSTEM32 hardening in any way.
struct NVMLGPUProbeTestAccessor
{
    static void inject(NVMLGPUProbe& probe, const NVMLGPUProbe::NVMLFunctions& fns, bool initialized)
    {
        // The constructor already ran the real loadNVML()/initializeNVML() against whatever
        // NVML is actually present on this machine. On a machine with a real NVIDIA driver
        // installed, that leaves a real nvmlInit() outstanding and a real DLL handle open;
        // tear both down properly (matching real nvmlShutdown() to the real nvmlInit(), and
        // freeing the real DLL) before substituting the fake backend below, so this doesn't
        // leak an outstanding initialization or an unmatched DLL reference count.
        probe.shutdownNVML();
        probe.unloadNVML();

        probe.m_NVML = fns;
        probe.m_Initialized = initialized;
    }

    static void addDevice(NVMLGPUProbe& probe, uint32_t index, NVML::nvmlDevice_t handle)
    {
        probe.m_DeviceHandles[index] = handle;
    }

    [[nodiscard]] static std::string errorString(NVML::nvmlReturn_t result)
    {
        return NVMLGPUProbe::getNVMLErrorString(result);
    }

    /// Builds a fully-populated NVMLFunctions table pointing at the fakes above. Tests null
    /// out individual fields (e.g. per-process functions) to exercise "not available" branches.
    [[nodiscard]] static NVMLGPUProbe::NVMLFunctions fullFakeFunctions()
    {
        NVMLGPUProbe::NVMLFunctions fns{};
        fns.Shutdown = fakeShutdown;
        fns.DeviceGetCount = fakeDeviceGetCount;
        fns.DeviceGetHandleByIndex = fakeDeviceGetHandleByIndex;
        fns.DeviceGetName = fakeDeviceGetName;
        fns.DeviceGetUUID = fakeDeviceGetUUID;
        fns.DeviceGetMemoryInfo = fakeDeviceGetMemoryInfo;
        fns.DeviceGetTemperature = fakeDeviceGetTemperature;
        fns.DeviceGetPowerUsage = fakeDeviceGetPowerUsage;
        fns.DeviceGetPowerManagementLimit = fakeDeviceGetPowerManagementLimit;
        fns.DeviceGetClockInfo = fakeDeviceGetClockInfo;
        fns.DeviceGetUtilizationRates = fakeDeviceGetUtilizationRates;
        fns.DeviceGetVbiosVersion = fakeDeviceGetVbiosVersion;
        fns.DeviceGetFanSpeed = fakeDeviceGetFanSpeed;
        fns.DeviceGetComputeRunningProcesses = fakeDeviceGetComputeRunningProcesses;
        fns.DeviceGetGraphicsRunningProcesses = fakeDeviceGetGraphicsRunningProcesses;
        return fns;
    }
};

namespace
{

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
}

TEST_F(NVMLGPUProbeFakeTest, ReadGPUCountersLeavesFieldsAtDefaultOnPerMetricFailure)
{
    auto& d = deviceData(0);
    d.temperatureOk = false;
    d.powerOk = false;
    d.utilizationOk = false;
    d.fanOk = false;

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
    fns.DeviceGetComputeRunningProcesses = nullptr;
    fns.DeviceGetGraphicsRunningProcesses = nullptr;
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

// ==========================================================================
// capabilities
// ==========================================================================

TEST_F(NVMLGPUProbeFakeTest, CapabilitiesReportsPerProcessMetricsWhenEitherFunctionAvailable)
{
    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetGraphicsRunningProcesses = nullptr; // only compute available
    NVMLGPUProbeTestAccessor::inject(probe, fns, /*initialized=*/true);

    EXPECT_TRUE(probe.capabilities().hasPerProcessMetrics);
}

TEST_F(NVMLGPUProbeFakeTest, CapabilitiesReportsNoPerProcessMetricsWhenNeitherAvailable)
{
    NVMLGPUProbe probe;
    auto fns = NVMLGPUProbeTestAccessor::fullFakeFunctions();
    fns.DeviceGetComputeRunningProcesses = nullptr;
    fns.DeviceGetGraphicsRunningProcesses = nullptr;
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
