#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GPUTypes.h"
#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/NVMLGPUProbe.h"
#include "Platform/Linux/NVMLGPUProbeMath.h"
#include "Platform/NVMLTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include <dlfcn.h>

namespace Platform
{
namespace
{

// NVMLGPUProbeMath: pure logic exercised with fake function pointers, covering the
// missing-symbol and null-string branches the fixed-behavior mock library
// (tests/Mocks/NVMLMock.cpp) never takes -- it always succeeds, so these paths are
// otherwise unreachable via NVMLGPUProbe itself.

TEST(NVMLGPUProbeMathTest, ResolveErrorStringReturnsLibraryStringWhenAvailable)
{
    const auto result = NVMLGPUProbeMath::resolveErrorString(NVML::NVML_ERROR_NOT_SUPPORTED,
                                                             [](NVML::nvmlReturn_t) -> const char* { return "Not Supported"; });
    EXPECT_EQ(result, "Not Supported");
}

TEST(NVMLGPUProbeMathTest, ResolveErrorStringFallsBackWhenFunctionPointerIsNull)
{
    EXPECT_EQ(NVMLGPUProbeMath::resolveErrorString(NVML::NVML_ERROR_NOT_SUPPORTED, nullptr), "Unknown NVML error");
}

TEST(NVMLGPUProbeMathTest, ResolveErrorStringFallsBackWhenLibraryReturnsNull)
{
    const auto result =
        NVMLGPUProbeMath::resolveErrorString(NVML::NVML_ERROR_NOT_SUPPORTED, [](NVML::nvmlReturn_t) -> const char* { return nullptr; });
    EXPECT_EQ(result, "Unknown NVML error");
}

// queryRunningProcesses (#1092): a fake entry point that behaves like NVML's.

struct FakeProcess
{
    unsigned int pid;
    std::uint64_t usedGpuMemory;
};

/// Writes `processes` with `entrySize` stride (pid at 0, usedGpuMemory at 8), answering a
/// too-small or null buffer with INSUFFICIENT_SIZE. `listedAfterCount` simulates processes that
/// start between the count query and the sized call.
struct FakeRunningProcesses
{
    std::vector<FakeProcess> processes;
    std::size_t entrySize = NVMLGPUProbeMath::kProcessInfoV2Size;
    std::vector<FakeProcess> listedAfterCount;
    NVML::nvmlReturn_t countOnlyResult = NVML::NVML_ERROR_INSUFFICIENT_SIZE;
    int calls = 0;

    NVML::nvmlReturn_t operator()(unsigned int* count, void* buffer)
    {
        ++calls;
        if (calls == 2)
        {
            processes.insert(processes.end(), listedAfterCount.begin(), listedAfterCount.end());
        }
        const unsigned int capacity = *count;
        *count = static_cast<unsigned int>(processes.size());
        if (buffer == nullptr)
        {
            return processes.empty() ? NVML::NVML_SUCCESS : countOnlyResult;
        }
        if (capacity < processes.size())
        {
            return NVML::NVML_ERROR_INSUFFICIENT_SIZE;
        }
        auto* bytes = static_cast<std::byte*>(buffer);
        for (std::size_t i = 0; i < processes.size(); ++i)
        {
            std::memcpy(bytes + (i * entrySize), &processes[i].pid, sizeof(unsigned int));
            std::memcpy(bytes + (i * entrySize) + 8, &processes[i].usedGpuMemory, sizeof(std::uint64_t));
        }
        return NVML::NVML_SUCCESS;
    }
};

FakeRunningProcesses makeFake(std::vector<FakeProcess> processes, std::size_t entrySize = NVMLGPUProbeMath::kProcessInfoV2Size)
{
    FakeRunningProcesses fake;
    fake.processes = std::move(processes);
    fake.entrySize = entrySize;
    return fake;
}

TEST(NVMLGPUProbeMathTest, NoRunningProcessesIsEmpty)
{
    FakeRunningProcesses fake;
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    EXPECT_TRUE(NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize).empty());
}

TEST(NVMLGPUProbeMathTest, InsufficientSizeOnCountQueryMeansProcessesAreRunning)
{
    auto fake = makeFake({{.pid = 10, .usedGpuMemory = 100}, {.pid = 20, .usedGpuMemory = 200}});
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize);

    ASSERT_EQ(processes.size(), 2U);
    EXPECT_EQ(processes[1].pid, 20U);
    EXPECT_EQ(processes[1].usedGpuMemoryBytes, 200U);
}

TEST(NVMLGPUProbeMathTest, LegacyV1EntriesAreReadAtTheirOwnStride)
{
    auto fake = makeFake({{.pid = 10, .usedGpuMemory = 100}, {.pid = 20, .usedGpuMemory = 200}, {.pid = 30, .usedGpuMemory = 300}},
                         NVMLGPUProbeMath::kProcessInfoV1Size);
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, NVMLGPUProbeMath::kProcessInfoV1Size);

    ASSERT_EQ(processes.size(), 3U);
    EXPECT_EQ(processes[2].pid, 30U);
    EXPECT_EQ(processes[2].usedGpuMemoryBytes, 300U);
}

TEST(NVMLGPUProbeMathTest, RetriesWhenTheListGrowsBetweenCalls)
{
    auto fake = makeFake({{.pid = 1, .usedGpuMemory = 1}});
    for (unsigned int pid = 2; pid <= 10; ++pid)
    {
        fake.listedAfterCount.push_back({.pid = pid, .usedGpuMemory = pid});
    }
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize);

    ASSERT_EQ(processes.size(), 10U);
    EXPECT_EQ(processes.back().pid, 10U);
}

TEST(NVMLGPUProbeMathTest, UnavailableMemoryIsNullopt)
{
    auto fake = makeFake({{.pid = 7, .usedGpuMemory = NVMLGPUProbeMath::kValueNotAvailable}});
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize);

    ASSERT_EQ(processes.size(), 1U);
    EXPECT_FALSE(processes[0].usedGpuMemoryBytes.has_value());
}

TEST(NVMLGPUProbeMathTest, ImplausibleProcessCountIsNotAllocated)
{
    // A corrupt count must not drive repeated multi-gigabyte allocations (#1213 review).
    int sizedCalls = 0;
    const auto query = [&sizedCalls](unsigned int* count, void* buffer)
    {
        if (buffer != nullptr)
        {
            ++sizedCalls;
        }
        *count = NVMLGPUProbeMath::kMaxPlausibleProcessCount + 1U;
        return NVML::NVML_ERROR_INSUFFICIENT_SIZE;
    };
    EXPECT_TRUE(NVMLGPUProbeMath::queryRunningProcesses(query, NVMLGPUProbeMath::kProcessInfoV2Size).empty());
    EXPECT_EQ(sizedCalls, 0);
}

TEST(NVMLGPUProbeMathTest, CountQueryErrorIsEmpty)
{
    auto fake = makeFake({{.pid = 1, .usedGpuMemory = 1}});
    fake.countOnlyResult = NVML::NVML_ERROR_NO_PERMISSION;
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    EXPECT_TRUE(NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize).empty());
}

TEST(NVMLGPUProbeMathTest, CountQueryReportingSuccessWithACountStillFetchesTheList)
{
    // Some drivers answer the count-only call with SUCCESS and a non-zero count.
    auto fake = makeFake({{.pid = 5, .usedGpuMemory = 50}});
    fake.countOnlyResult = NVML::NVML_SUCCESS;
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, fake.entrySize);

    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].pid, 5U);
}

TEST(LinuxNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe;
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxNVMLGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    NVMLGPUProbe probe;
    if (probe.isAvailable())
    {
        // Under CTest (ENVIRONMENT_MODIFICATION), LD_LIBRARY_PATH is prepended with the
        // mock library so the probe is always available. The unavailable path cannot be
        // exercised in this process.
        GTEST_SKIP() << "NVML probe is available (mock library loaded); unavailable path not testable in this environment";
    }

    const auto caps = probe.capabilities();
    EXPECT_FALSE(caps.hasTemperature);
    EXPECT_FALSE(caps.hasPowerMetrics);
    EXPECT_FALSE(caps.hasClockSpeeds);
    EXPECT_FALSE(caps.hasFanSpeed);
    EXPECT_FALSE(caps.hasPCIeMetrics);
    EXPECT_FALSE(caps.hasPerProcessMetrics);
    EXPECT_FALSE(caps.supportsMultiGPU);
    EXPECT_FALSE(caps.hasEngineUtilization);
}

TEST(LinuxNVMLGPUProbeTest, AvailableProbeReturnsConsistentIds)
{
    NVMLGPUProbe probe;
    if (probe.isAvailable())
    {
        const auto gpus = probe.enumerateGPUs();
        const auto counters = probe.readGPUCounters();
        for (const auto& gpu : gpus)
        {
            EXPECT_FALSE(gpu.id.empty());
        }
        for (const auto& counter : counters)
        {
            EXPECT_FALSE(counter.gpuId.empty());
        }
    }
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryEnablesAvailableCapabilities)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto caps = probe.capabilities();
    EXPECT_TRUE(caps.hasTemperature);
    EXPECT_TRUE(caps.hasPowerMetrics);
    EXPECT_TRUE(caps.hasClockSpeeds);
    EXPECT_TRUE(caps.hasFanSpeed);
    // NVML only returns PCIe throughput as rates, not cumulative counters, so this probe
    // deliberately reports the capability as unavailable rather than present-but-always-zero.
    EXPECT_FALSE(caps.hasPCIeMetrics);
    EXPECT_TRUE(caps.hasPerProcessMetrics);
    EXPECT_TRUE(caps.supportsMultiGPU);
    EXPECT_TRUE(caps.hasEngineUtilization);
    EXPECT_FALSE(caps.hasHotspotTemp);
    EXPECT_FALSE(caps.hasEncoderDecoder);
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryEnumeratesDevicesAndUsesUuidFallback)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);

    EXPECT_EQ(gpus[0].vendor, "NVIDIA");
    EXPECT_EQ(gpus[0].name, "Mock NVIDIA GPU 0");
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_FALSE(gpus[0].isIntegrated);
    EXPECT_EQ(gpus[0].deviceIndex, 0U);

    EXPECT_EQ(gpus[1].vendor, "NVIDIA");
    EXPECT_EQ(gpus[1].name, "Mock NVIDIA GPU 1");
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    EXPECT_FALSE(gpus[1].isIntegrated);
    EXPECT_EQ(gpus[1].deviceIndex, 1U);
}

/// Drives the NVML mock's test controls (tasksmackNvmlMockConfigure) for one test and resets
/// them afterwards. dlopen() returns the same instance the probe loads.
class NvmlMockControls
{
  public:
    NvmlMockControls() : m_Library(dlopen("libnvidia-ml.so.1", RTLD_NOW))
    {
        if (m_Library != nullptr)
        {
            // dlsym returns void* by POSIX definition; the casts restore the mock's signatures.
            // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
            m_Configure = reinterpret_cast<ConfigureFn>(dlsym(m_Library, "tasksmackNvmlMockConfigure"));
            m_UuidCalls = reinterpret_cast<UuidCallsFn>(dlsym(m_Library, "tasksmackNvmlMockUuidCalls"));
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        }
    }

    ~NvmlMockControls()
    {
        if (m_Configure != nullptr)
        {
            m_Configure(NO_FAILING_HANDLE, -1);
        }
        if (m_Library != nullptr)
        {
            dlclose(m_Library);
        }
    }

    NvmlMockControls(const NvmlMockControls&) = delete;
    NvmlMockControls& operator=(const NvmlMockControls&) = delete;
    NvmlMockControls(NvmlMockControls&&) = delete;
    NvmlMockControls& operator=(NvmlMockControls&&) = delete;

    [[nodiscard]] bool available() const
    {
        return m_Configure != nullptr && m_UuidCalls != nullptr;
    }

    void configure(unsigned int failingHandleIndex, int uuidCallsBeforeFailure) const
    {
        m_Configure(failingHandleIndex, uuidCallsBeforeFailure);
    }

    [[nodiscard]] unsigned int uuidCalls() const
    {
        return m_UuidCalls();
    }

    static constexpr unsigned int NO_FAILING_HANDLE = std::numeric_limits<unsigned int>::max();

  private:
    using ConfigureFn = void (*)(unsigned int, int);
    using UuidCallsFn = unsigned int (*)();

    void* m_Library;
    ConfigureFn m_Configure = nullptr;
    UuidCallsFn m_UuidCalls = nullptr;
};

// #1162: a device whose handle NVML won't return is skipped, not sampled through a null handle
// as an all-zero phantom GPU.
TEST(LinuxNVMLGPUProbeTest, DeviceWithoutAHandleIsSkipped)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.available());
    controls.configure(0, -1);

    NVMLGPUProbe probe;
    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].deviceIndex, 1U);
    EXPECT_EQ(gpus[0].id, "nvidia-1");

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuId, "nvidia-1");
}

// #1162: ids are resolved once at load, so a UUID failure afterwards can't turn a GPU into a
// different "nvidia-N" id for one sample, and sampling makes no further UUID calls.
TEST(LinuxNVMLGPUProbeTest, DeviceIdsAreResolvedOnceAtLoad)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.available());
    controls.configure(NvmlMockControls::NO_FAILING_HANDLE, 2); // every UUID call after load fails

    NVMLGPUProbe probe;
    ASSERT_TRUE(probe.isAvailable());
    const unsigned int callsAtLoad = controls.uuidCalls();

    const auto counters = probe.readGPUCounters();
    const auto processCounters = probe.readProcessGPUCounters();
    const auto gpus = probe.enumerateGPUs();

    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[0].gpuId, "mock-nvml-uuid-0");
    ASSERT_FALSE(processCounters.empty());
    EXPECT_EQ(processCounters.front().gpuId, "mock-nvml-uuid-0");
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(controls.uuidCalls(), callsAtLoad);
}

TEST(LinuxNVMLGPUProbeTest, MockLibraryReturnsExpectedCountersAndMergesProcessEngines)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe;

    ASSERT_TRUE(probe.isAvailable());

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);

    EXPECT_EQ(counters[0].gpuId, "mock-nvml-uuid-0");
    EXPECT_DOUBLE_EQ(counters[0].utilizationPercent, 75.0);
    EXPECT_EQ(counters[0].memoryUsedBytes, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].memoryTotalBytes, 8ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(counters[0].temperatureC, 65);
    EXPECT_DOUBLE_EQ(counters[0].powerDrawWatts, 125.0);
    EXPECT_DOUBLE_EQ(counters[0].powerLimitWatts, 250.0);
    EXPECT_EQ(counters[0].gpuClockMHz, 1800U);
    EXPECT_EQ(counters[0].memoryClockMHz, 9000U);
    EXPECT_EQ(counters[0].fanSpeedRaw, 40U);
    EXPECT_EQ(counters[0].fanSpeedMaxRaw, 100U); // NVML already returns 0-100
    // nvmlDeviceGetPcieThroughput() returns a rate (KB/s over a ~20ms window), not a
    // cumulative counter, so the probe deliberately leaves these at their zero default
    // rather than populate a field GPUModel treats as cumulative with mismatched data.
    EXPECT_EQ(counters[0].pcieTxBytes, 0U);
    EXPECT_EQ(counters[0].pcieRxBytes, 0U);

    EXPECT_EQ(counters[1].gpuId, "nvidia-1");
    EXPECT_DOUBLE_EQ(counters[1].utilizationPercent, 25.0);

    // The mock answers like real NVML: INSUFFICIENT_SIZE for the count query, the _v3 stride, and
    // NVML_VALUE_NOT_AVAILABLE for one process's memory (#1092).
    const auto processCounters = probe.readProcessGPUCounters();
    ASSERT_EQ(processCounters.size(), 3U);

    const auto merged = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 123; });
    ASSERT_NE(merged, processCounters.end());
    EXPECT_EQ(merged->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(merged->gpuMemoryBytes, 222U);
    ASSERT_EQ(merged->activeEngines.size(), 2U);
    EXPECT_EQ(merged->activeEngines[0], "Compute");
    EXPECT_EQ(merged->activeEngines[1], "3D");

    const auto graphicsOnly = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 456; });
    ASSERT_NE(graphicsOnly, processCounters.end());
    EXPECT_EQ(graphicsOnly->gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(graphicsOnly->gpuMemoryBytes, 333U);
    ASSERT_EQ(graphicsOnly->activeEngines.size(), 1U);
    EXPECT_EQ(graphicsOnly->activeEngines[0], "3D");

    // Memory NVML can't report is not passed on as ~16 EiB.
    const auto noMemory = std::ranges::find_if(processCounters, [](const ProcessGPUCounters& counter) { return counter.pid == 789; });
    ASSERT_NE(noMemory, processCounters.end());
    EXPECT_EQ(noMemory->gpuMemoryBytes, 0U);
}

} // namespace
} // namespace Platform

#endif
