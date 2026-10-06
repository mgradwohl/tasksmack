#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/GPUTypes.h"
#include "Platform/GpuMockLibraryTestUtils.h"
#include "Platform/Linux/NVMLGPUProbe.h"
#include "Platform/Linux/NVMLGPUProbeMath.h"
#include "Platform/Linux/PciDisplayDevices.h"
#include "Platform/Linux/PciRuntimePm.h"
#include "Platform/NVMLTypes.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
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
    std::size_t entrySize = NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE;
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

FakeRunningProcesses makeFake(std::vector<FakeProcess> processes, std::size_t entrySize = NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE)
{
    FakeRunningProcesses fake;
    fake.processes = std::move(processes);
    fake.entrySize = entrySize;
    return fake;
}

// chooseRunningProcessesSymbol (#1092): which entry point the loader picks, and its entry size.
// The mock library exports every variant, so the loader's fallback is tested through a resolver.

/// A resolver exporting only `exported`; returns a distinct non-null address for each.
struct FakeResolver
{
    std::vector<std::string> exported;

    void* operator()(const std::string& name) const
    {
        for (std::size_t i = 0; i < exported.size(); ++i)
        {
            if (exported[i] == name)
            {
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr) - opaque fake address
                return reinterpret_cast<void*>(i + 1);
            }
        }
        return nullptr;
    }
};

TEST(NVMLGPUProbeMathTest, PrefersTheV3RunningProcessesSymbol)
{
    const FakeResolver resolver{
        {"nvmlDeviceGetComputeRunningProcesses", "nvmlDeviceGetComputeRunningProcesses_v2", "nvmlDeviceGetComputeRunningProcesses_v3"}};
    const auto symbol = NVMLGPUProbeMath::chooseRunningProcessesSymbol("nvmlDeviceGetComputeRunningProcesses", resolver);
    EXPECT_EQ(symbol.name, "nvmlDeviceGetComputeRunningProcesses_v3");
    EXPECT_EQ(symbol.entrySize, NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE);
}

TEST(NVMLGPUProbeMathTest, FallsBackToTheV2RunningProcessesSymbol)
{
    const FakeResolver resolver{{"nvmlDeviceGetGraphicsRunningProcesses", "nvmlDeviceGetGraphicsRunningProcesses_v2"}};
    const auto symbol = NVMLGPUProbeMath::chooseRunningProcessesSymbol("nvmlDeviceGetGraphicsRunningProcesses", resolver);
    EXPECT_EQ(symbol.name, "nvmlDeviceGetGraphicsRunningProcesses_v2");
    EXPECT_EQ(symbol.entrySize, NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE);
}

TEST(NVMLGPUProbeMathTest, LegacyRunningProcessesSymbolUsesTheV1EntrySize)
{
    // An older driver exports only the unversioned symbol, which writes 16-byte entries.
    const FakeResolver resolver{{"nvmlDeviceGetComputeRunningProcesses"}};
    const auto symbol = NVMLGPUProbeMath::chooseRunningProcessesSymbol("nvmlDeviceGetComputeRunningProcesses", resolver);
    EXPECT_EQ(symbol.name, "nvmlDeviceGetComputeRunningProcesses");
    EXPECT_NE(symbol.address, nullptr);
    EXPECT_EQ(symbol.entrySize, NVMLGPUProbeMath::PROCESS_INFO_V1_SIZE);
}

TEST(NVMLGPUProbeMathTest, NoRunningProcessesSymbolExported)
{
    const FakeResolver resolver{};
    const auto symbol = NVMLGPUProbeMath::chooseRunningProcessesSymbol("nvmlDeviceGetComputeRunningProcesses", resolver);
    EXPECT_EQ(symbol.address, nullptr);
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
                         NVMLGPUProbeMath::PROCESS_INFO_V1_SIZE);
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        return fake(count, buffer);
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, NVMLGPUProbeMath::PROCESS_INFO_V1_SIZE);

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
    auto fake = makeFake({{.pid = 7, .usedGpuMemory = NVMLGPUProbeMath::VALUE_NOT_AVAILABLE}});
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
        *count = NVMLGPUProbeMath::MAX_PLAUSIBLE_PROCESS_COUNT + 1U;
        return NVML::NVML_ERROR_INSUFFICIENT_SIZE;
    };
    EXPECT_TRUE(NVMLGPUProbeMath::queryRunningProcesses(query, NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE).empty());
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

// combineRunningProcesses (#1213 review): one entry per process, MIG instances summed.

NVMLGPUProbeMath::RunningProcess
onInstance(std::uint32_t pid, std::uint64_t memory, std::uint32_t gpuInstance, std::uint32_t computeInstance)
{
    return {.pid = pid, .usedGpuMemoryBytes = memory, .gpuInstanceId = gpuInstance, .computeInstanceId = computeInstance};
}

TEST(NVMLGPUProbeMathTest, MigInstancesOfOneProcessAreSummed)
{
    // PID 5 runs on two MIG instances; graphics reports instance (1, 0) again, a little higher.
    const std::vector compute{onInstance(5, 100, 1, 0), onInstance(5, 200, 2, 0)};
    const std::vector graphics{onInstance(5, 150, 1, 0)};

    const auto usages = NVMLGPUProbeMath::combineRunningProcesses(compute, graphics);
    ASSERT_EQ(usages.size(), 1U);
    EXPECT_EQ(usages[0].memoryBytes, 350U); // max(100, 150) on instance (1, 0) + 200 on (2, 0)
    EXPECT_TRUE(usages[0].compute);
    EXPECT_TRUE(usages[0].graphics);
}

TEST(NVMLGPUProbeMathTest, LegacyEntriesForOneProcessTakeTheLarger)
{
    // 16-byte entries carry no instance ids: compute and graphics are one allocation.
    const std::vector<NVMLGPUProbeMath::RunningProcess> compute{{.pid = 7, .usedGpuMemoryBytes = 100}};
    const std::vector<NVMLGPUProbeMath::RunningProcess> graphics{{.pid = 7, .usedGpuMemoryBytes = 120},
                                                                 {.pid = 8, .usedGpuMemoryBytes = 50}};

    const auto usages = NVMLGPUProbeMath::combineRunningProcesses(compute, graphics);
    ASSERT_EQ(usages.size(), 2U);
    EXPECT_EQ(usages[0].pid, 7U);
    EXPECT_EQ(usages[0].memoryBytes, 120U);
    EXPECT_EQ(usages[1].pid, 8U);
    EXPECT_FALSE(usages[1].compute);
}

TEST(NVMLGPUProbeMathTest, InstanceIdsAreReadFromV2Entries)
{
    auto fake = makeFake({{.pid = 9, .usedGpuMemory = 64}});
    const auto query = [&fake](unsigned int* count, void* buffer)
    {
        const auto result = fake(count, buffer);
        if (buffer != nullptr && result == NVML::NVML_SUCCESS)
        {
            const std::uint32_t gpuInstance = 3;
            const std::uint32_t computeInstance = 1;
            std::memcpy(
                static_cast<std::byte*>(buffer) + NVMLGPUProbeMath::PROCESS_INFO_GPU_INSTANCE_OFFSET, &gpuInstance, sizeof(gpuInstance));
            std::memcpy(static_cast<std::byte*>(buffer) + NVMLGPUProbeMath::PROCESS_INFO_COMPUTE_INSTANCE_OFFSET,
                        &computeInstance,
                        sizeof(computeInstance));
        }
        return result;
    };
    const auto processes = NVMLGPUProbeMath::queryRunningProcesses(query, NVMLGPUProbeMath::PROCESS_INFO_V2_SIZE);
    ASSERT_EQ(processes.size(), 1U);
    EXPECT_EQ(processes[0].gpuInstanceId, 3U);
    EXPECT_EQ(processes[0].computeInstanceId, 1U);
}

TEST(LinuxNVMLGPUProbeTest, BasicOperationsDoNotThrow)
{
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    EXPECT_NO_THROW([[maybe_unused]] auto available = probe.isAvailable());
    EXPECT_NO_THROW([[maybe_unused]] auto gpus = probe.enumerateGPUs());
    EXPECT_NO_THROW([[maybe_unused]] auto counters = probe.readGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto process = probe.readProcessGPUCounters());
    EXPECT_NO_THROW([[maybe_unused]] auto caps = probe.capabilities());
}

TEST(LinuxNVMLGPUProbeTest, UnavailableProbeReportsNoCapabilities)
{
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
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
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
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
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

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
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

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
            m_FailSensorReads = reinterpret_cast<FailSensorReadsFn>(dlsym(m_Library, "tasksmackNvmlMockFailSensorReads"));
            m_DeviceQueries = reinterpret_cast<UuidCallsFn>(dlsym(m_Library, "tasksmackNvmlMockDeviceQueries"));
            m_SetDeviceCount = reinterpret_cast<SetIndexFn>(dlsym(m_Library, "tasksmackNvmlMockSetDeviceCount"));
            m_SetLostDevice = reinterpret_cast<SetIndexFn>(dlsym(m_Library, "tasksmackNvmlMockSetLostDevice"));
            m_InitCalls = reinterpret_cast<UuidCallsFn>(dlsym(m_Library, "tasksmackNvmlMockInitCalls"));
            m_FailInits = reinterpret_cast<SetIndexFn>(dlsym(m_Library, "tasksmackNvmlMockFailInits"));
            m_QueriesForDevice = reinterpret_cast<QueriesForDeviceFn>(dlsym(m_Library, "tasksmackNvmlMockQueriesForDevice"));
            // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
        }
    }

    ~NvmlMockControls()
    {
        if (m_Configure != nullptr)
        {
            m_Configure(NO_FAILING_HANDLE, -1);
        }
        if (m_FailSensorReads != nullptr)
        {
            m_FailSensorReads(0);
        }
        if (m_SetDeviceCount != nullptr)
        {
            m_SetDeviceCount(MOCK_DEVICE_COUNT);
        }
        if (m_SetLostDevice != nullptr)
        {
            m_SetLostDevice(NO_FAILING_HANDLE);
        }
        if (m_FailInits != nullptr)
        {
            m_FailInits(0);
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
        return m_Configure != nullptr && m_UuidCalls != nullptr && m_FailSensorReads != nullptr;
    }

    void failSensorReads(bool fail) const
    {
        m_FailSensorReads(fail ? 1 : 0);
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
    static constexpr unsigned int MOCK_DEVICE_COUNT = 2;

    /// #1116 controls: whether the mock has them, and the controls themselves.
    [[nodiscard]] bool controlsDeviceSet() const
    {
        return m_SetDeviceCount != nullptr && m_SetLostDevice != nullptr && m_InitCalls != nullptr;
    }

    /// NVML reports the first `count` mock devices from its next device count on.
    void setDeviceCount(unsigned int count) const
    {
        m_SetDeviceCount(count);
    }

    /// Mock device `index`'s sensor reads return NVML_ERROR_GPU_IS_LOST (NO_FAILING_HANDLE: none).
    void setLostDevice(unsigned int index) const
    {
        m_SetLostDevice(index);
    }

    [[nodiscard]] unsigned int initCalls() const
    {
        return m_InitCalls();
    }

    /// Whether the mock can fail nvmlInit_v2.
    [[nodiscard]] bool controlsInit() const
    {
        return m_FailInits != nullptr && m_InitCalls != nullptr;
    }

    /// The next `count` nvmlInit_v2 calls fail with NVML_ERROR_DRIVER_NOT_LOADED.
    void failInits(unsigned int count) const
    {
        m_FailInits(count);
    }

  private:
    using ConfigureFn = void (*)(unsigned int, int);
    using UuidCallsFn = unsigned int (*)();
    using FailSensorReadsFn = void (*)(int);
    using SetIndexFn = void (*)(unsigned int);
    using QueriesForDeviceFn = unsigned int (*)(unsigned int);

    void* m_Library;
    QueriesForDeviceFn m_QueriesForDevice = nullptr;
    ConfigureFn m_Configure = nullptr;
    UuidCallsFn m_UuidCalls = nullptr;
    FailSensorReadsFn m_FailSensorReads = nullptr;
    UuidCallsFn m_DeviceQueries = nullptr;
    SetIndexFn m_SetDeviceCount = nullptr;
    SetIndexFn m_SetLostDevice = nullptr;
    UuidCallsFn m_InitCalls = nullptr;
    SetIndexFn m_FailInits = nullptr;

  public:
    /// Calls that have addressed a device so far (NVML mock's tasksmackNvmlMockDeviceQueries).
    [[nodiscard]] unsigned int deviceQueries() const
    {
        return m_DeviceQueries != nullptr ? m_DeviceQueries() : 0U;
    }

    [[nodiscard]] bool countsDeviceQueries() const
    {
        return m_DeviceQueries != nullptr;
    }

    /// Calls that have addressed mock device `index`, handle lookups included (#1270).
    [[nodiscard]] unsigned int queriesForDevice(unsigned int index) const
    {
        return m_QueriesForDevice != nullptr ? m_QueriesForDevice(index) : 0U;
    }

    [[nodiscard]] bool countsQueriesPerDevice() const
    {
        return m_QueriesForDevice != nullptr;
    }
};

// #1162: a device whose handle NVML won't return is skipped, not sampled through a null handle
// as an all-zero phantom GPU.
// #1111: a sensor read that fails (timeout, GPU lost, driver reset) is marked unread, not reported
// as a real-looking 0.
TEST(LinuxNVMLGPUProbeTest, FailedSensorReadsAreMarkedUnavailable)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.available());

    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());

    const auto healthy = probe.readGPUCounters();
    ASSERT_FALSE(healthy.empty());
    EXPECT_TRUE(healthy[0].utilizationAvailable);
    EXPECT_TRUE(healthy[0].temperatureAvailable);
    EXPECT_TRUE(healthy[0].powerAvailable);
    EXPECT_TRUE(healthy[0].gpuClockAvailable);

    controls.failSensorReads(true);
    const auto failed = probe.readGPUCounters();
    ASSERT_FALSE(failed.empty());
    EXPECT_FALSE(failed[0].utilizationAvailable);
    EXPECT_FALSE(failed[0].temperatureAvailable);
    EXPECT_FALSE(failed[0].powerAvailable);
    EXPECT_FALSE(failed[0].gpuClockAvailable);
    EXPECT_FALSE(failed[0].memoryAvailable);
    EXPECT_EQ(failed[0].memoryClockMHz, 9000U); // reads that still succeed are unaffected
}

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

    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
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

    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
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
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);

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

// =============================================================================
// Runtime PM helpers (#1117)
// =============================================================================

TEST(PciRuntimePmTest, OnlySuspendedAndSuspendingCountAsAsleep)
{
    EXPECT_TRUE(PciRuntimePm::isSuspendedStatus("suspended"));
    EXPECT_TRUE(PciRuntimePm::isSuspendedStatus("suspended\n"));
    EXPECT_TRUE(PciRuntimePm::isSuspendedStatus("suspending"));
    EXPECT_FALSE(PciRuntimePm::isSuspendedStatus("active"));
    EXPECT_FALSE(PciRuntimePm::isSuspendedStatus("resuming"));
    EXPECT_FALSE(PciRuntimePm::isSuspendedStatus("unsupported"));
    EXPECT_FALSE(PciRuntimePm::isSuspendedStatus(""));
}

TEST(PciRuntimePmTest, UnreadableStatusIsAwake)
{
    EXPECT_FALSE(PciRuntimePm::isRuntimeSuspended(""));
    EXPECT_FALSE(PciRuntimePm::isRuntimeSuspended("/nonexistent/tasksmack/pci/0000:01:00.0"));
}

TEST(NVMLGPUProbeMathTest, SysfsPciAddressUsesTheKernelsFourDigitDomain)
{
    NVML::nvmlPciInfo_t pci{};
    pci.domain = 0;
    pci.bus = 0x01;
    pci.device = 0;
    std::strncpy(std::data(pci.busId), "00000000:01:00.0", std::size(pci.busId) - 1);
    EXPECT_EQ(NVMLGPUProbeMath::sysfsPciAddress(pci), "0000:01:00.0");

    pci.domain = 0x10000; // wider domains keep every digit, as sysfs prints them
    pci.bus = 0xc1;
    pci.device = 0x1f;
    std::strncpy(std::data(pci.busId), "00010000:C1:1F.3", std::size(pci.busId) - 1);
    EXPECT_EQ(NVMLGPUProbeMath::sysfsPciAddress(pci), "10000:c1:1f.3");

    pci.busId[0] = '\0'; // no busId: function 0
    EXPECT_EQ(NVMLGPUProbeMath::sysfsPciAddress(pci), "10000:c1:1f.0");
}

// =============================================================================
// Per-device sensor capabilities (#1112) and runtime PM (#1117)
// =============================================================================

// #1272 review: a transient failure (NVML_ERROR_TIMEOUT) while sensors are probed at enumeration
// doesn't hide them for the session; only NVML_ERROR_NOT_SUPPORTED does. Once reads recover the
// sensors report values.
TEST(LinuxNVMLGPUProbeTest, ATransientFailureAtEnumerationKeepsTheSensors)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());

    controls.failSensorReads(true);
    const auto gpus = probe.enumerateGPUs();
    controls.failSensorReads(false);
    ASSERT_EQ(gpus.size(), 2U);
    const auto desktop = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(desktop.hasTemperature);
    EXPECT_TRUE(desktop.hasPowerMetrics);
    EXPECT_TRUE(desktop.hasClockSpeeds);
    const auto laptop = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_FALSE(laptop.hasPowerMetrics); // NOT_SUPPORTED still means unsupported

    const auto counters = probe.readGPUCounters();
    ASSERT_FALSE(counters.empty());
    EXPECT_TRUE(counters[0].temperatureAvailable);
}

// Like the Windows probe since #1040: device 1 (a laptop GPU in the mock) reports no power or fan,
// so its GPUInfo says so instead of inheriting NVML's probe-wide capabilities.
TEST(LinuxNVMLGPUProbeTest, SensorCapabilitiesArePerDevice)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);

    ASSERT_TRUE(gpus[0].sensorCapabilities.has_value());
    const auto desktop = gpus[0].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(desktop.hasTemperature);
    EXPECT_TRUE(desktop.hasPowerMetrics);
    EXPECT_TRUE(desktop.hasClockSpeeds);
    EXPECT_TRUE(desktop.hasFanSpeed);

    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    const auto laptop = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(laptop.hasTemperature);
    EXPECT_FALSE(laptop.hasPowerMetrics);
    EXPECT_TRUE(laptop.hasClockSpeeds);
    EXPECT_FALSE(laptop.hasFanSpeed);

    // The PCI identity is reported too.
    ASSERT_TRUE(gpus[1].pciLocation.has_value());
    EXPECT_EQ(gpus[1].pciLocation.value_or(PciLocation{}).bus, 0x41U);
}

// A runtime-suspended GPU is not queried at all (no sensor read, no process list), so the probe
// doesn't keep a hybrid laptop's dGPU awake; its readings are unavailable and it is marked asleep.
TEST(LinuxNVMLGPUProbeTest, RuntimeSuspendedGpuIsNotQueried)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.countsDeviceQueries());

    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_runtime_pm");
    const auto setStatus = [&pciRoot](const std::string& address, const std::string& status)
    {
        std::filesystem::create_directories(pciRoot.path / address / "power");
        std::ofstream(pciRoot.path / address / "power" / "runtime_status") << status << "\n";
    };
    setStatus("0000:01:00.0", "active");
    setStatus("0000:41:00.0", "active");

    NVMLGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto awake = probe.readGPUCounters(); // reads the memory totals while awake
    ASSERT_EQ(awake.size(), 2U);
    EXPECT_FALSE(awake[0].suspended);
    EXPECT_TRUE(awake[0].utilizationAvailable);

    setStatus("0000:01:00.0", "suspended");
    setStatus("0000:41:00.0", "suspended");
    const unsigned int queriesBefore = controls.deviceQueries();
    const auto asleep = probe.readGPUCounters();
    const auto processes = probe.readProcessGPUCounters();
    EXPECT_EQ(controls.deviceQueries(), queriesBefore); // nothing addressed a sleeping device

    ASSERT_EQ(asleep.size(), 2U);
    EXPECT_EQ(asleep[0].gpuId, "mock-nvml-uuid-0");
    EXPECT_TRUE(asleep[0].suspended);
    EXPECT_FALSE(asleep[0].utilizationAvailable);
    EXPECT_FALSE(asleep[0].temperatureAvailable);
    EXPECT_FALSE(asleep[0].powerAvailable);
    EXPECT_FALSE(asleep[0].gpuClockAvailable);
    EXPECT_FALSE(asleep[0].memoryAvailable);
    EXPECT_EQ(asleep[0].fanSpeedMaxRaw, 0U);
    EXPECT_EQ(asleep[0].memoryTotalBytes, 8ULL * 1024ULL * 1024ULL * 1024ULL); // last total read awake
    EXPECT_TRUE(processes.empty());

    // Enumerating doesn't wake it to probe sensors either: the probe's capabilities apply.
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_FALSE(gpus[0].sensorCapabilities.has_value());

    // Awake again: read as normal.
    setStatus("0000:01:00.0", "active");
    const auto woken = probe.readGPUCounters();
    ASSERT_EQ(woken.size(), 2U);
    EXPECT_FALSE(woken[0].suspended);
    EXPECT_DOUBLE_EQ(woken[0].utilizationPercent, 75.0);
    EXPECT_TRUE(woken[1].suspended);
}

// =============================================================================
// Re-enumeration (#1116, #1289, #1270)
// =============================================================================

/// A fake /sys/bus/pci/devices entry for a GPU: vendor and class attributes, a driver link, and
/// power/runtime_status.
void makePciDevice(const std::filesystem::path& root,
                   const std::string& address,
                   const std::string& vendor,
                   const std::string& driver,
                   const std::string& runtimeStatus = "active",
                   const std::string& pciClass = "0x030000")
{
    const auto dir = root / address;
    std::filesystem::create_directories(dir / "power");
    std::ofstream(dir / "vendor") << vendor << "\n";
    std::ofstream(dir / "class") << pciClass << "\n";
    std::ofstream(dir / "power" / "runtime_status") << runtimeStatus << "\n";
    if (!driver.empty())
    {
        std::filesystem::create_symlink("/nonexistent/drivers/" + driver, dir / "driver");
    }
}

void setRuntimeStatus(const std::filesystem::path& root, const std::string& address, const std::string& status)
{
    std::ofstream(root / address / "power" / "runtime_status") << status << "\n";
}

/// A fake /proc/driver/nvidia/gpus/<address>/information, laid out as the driver prints it (#1270).
/// An empty `uuid` leaves the line out, as the driver does before the GPU's UUID is cached.
void makeProcGpuInformation(const std::filesystem::path& root,
                            const std::string& address,
                            const std::string& model,
                            const std::string& uuid)
{
    std::filesystem::create_directories(root / address);
    std::ofstream file(root / address / "information");
    file << "Model: \t\t " << model << "\n" << "IRQ:   \t\t 142\n";
    if (!uuid.empty())
    {
        file << "GPU UUID: \t " << uuid << "\n";
    }
    file << "Video BIOS: \t ??.??.??.??.??\n" << "Bus Location: \t " << address << "\n";
}

TEST(PciDisplayDevicesTest, ListsOnlyTheVendorsDisplayDevicesWithTheirDriver)
{
    const TestSupport::ScopedTempDir pciRoot("tasksmack_pci_display");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:01:00.1", "0x10de", "snd_hda_intel", "active", "0x040300"); // HDMI audio
    makePciDevice(pciRoot.path, "0000:03:00.0", "0x1002", "amdgpu", "active", "0x038000");
    makePciDevice(pciRoot.path, "0000:00:02.0", "0x8086", "i915");

    EXPECT_EQ(PciDisplayDevices::list(pciRoot.path.string(), PciDisplayDevices::PCI_VENDOR_NVIDIA),
              (std::vector<std::string>{"0000:01:00.0=nvidia", "0000:41:00.0="}));
    EXPECT_EQ(PciDisplayDevices::list(pciRoot.path.string(), PciDisplayDevices::PCI_VENDOR_AMD),
              (std::vector<std::string>{"0000:03:00.0=amdgpu"}));
    EXPECT_TRUE(PciDisplayDevices::list(TestSupport::ISOLATED_PCI_ROOT, PciDisplayDevices::PCI_VENDOR_NVIDIA).empty());
}

// #1116: a GPU lost after a driver reset (NVML_ERROR_GPU_IS_LOST) gets NVML re-initialised at the next
// full rescan -- not on every sample -- and the rebuilt device list no longer has it.
TEST(LinuxNVMLGPUProbeTest, ALostGpuReinitialisesNvmlOnTheNextFullRescan)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsDeviceSet());

    NVMLGPUProbe probe(TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // nothing changed: no re-init
    const unsigned int initsBefore = controls.initCalls();

    // Device 1 falls off the bus: its reads report it lost, and NVML would no longer list it.
    controls.setLostDevice(1);
    controls.setDeviceCount(1);
    const auto lost = probe.readGPUCounters();
    ASSERT_EQ(lost.size(), 2U);
    EXPECT_FALSE(lost[1].utilizationAvailable);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // waits for the full rescan
    EXPECT_EQ(controls.initCalls(), initsBefore);

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 1U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0"); // the survivor keeps its id, so its history carries on
    EXPECT_EQ(gpus[0].name, "Mock NVIDIA GPU 0");
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 1U);
    EXPECT_EQ(counters[0].gpuId, "mock-nvml-uuid-0");

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // recovered: no further re-init
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);
}

// #1116: a GPU hot-plugged after startup (it appears under /sys/bus/pci/devices) gets NVML
// re-initialised at the next full rescan, and is enumerated beside the existing one, whose id stays.
TEST(LinuxNVMLGPUProbeTest, AHotPluggedGpuIsFoundOnTheNextFullRescan)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsDeviceSet());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_hotplug");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    controls.setDeviceCount(1);

    NVMLGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 1U);
    const unsigned int initsBefore = controls.initCalls();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));

    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");
    controls.setDeviceCount(2);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // a quick rescan doesn't scan sysfs
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    EXPECT_EQ(probe.readGPUCounters().size(), 2U);
}

// #1289: a GPU asleep at enumeration gets no sensor set of its own (it isn't woken to probe one). On
// its first awake sample a quick rescan asks for a re-enumeration, which finds its own sensors -- the
// mock's device 1 has no power or fan reading -- and later rescans ask for nothing more.
TEST(LinuxNVMLGPUProbeTest, AGpuAsleepAtEnumerationGetsItsOwnSensorsOnceAwake)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.countsDeviceQueries());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_wake");
    const TestSupport::ScopedTempDir procRoot("tasksmack_nvml_wake_proc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia", "suspended");
    makeProcGpuInformation(procRoot.path, "0000:41:00.0", "Mock NVIDIA GPU 1", "");

    NVMLGPUProbe probe(pciRoot.path.string(), procRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto asleep = probe.enumerateGPUs();
    ASSERT_EQ(asleep.size(), 2U);
    EXPECT_TRUE(asleep[0].sensorCapabilities.has_value());
    EXPECT_FALSE(asleep[1].sensorCapabilities.has_value());
    EXPECT_EQ(asleep[1].name, "Mock NVIDIA GPU 1"); // the driver's procfs name, not an NVML query

    const unsigned int queriesWhileAsleep = controls.deviceQueries();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // still asleep: nothing to find yet
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.deviceQueries(), queriesWhileAsleep); // and nothing addressed it

    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "active");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto awake = probe.enumerateGPUs();
    ASSERT_EQ(awake.size(), 2U);
    ASSERT_TRUE(awake[1].sensorCapabilities.has_value());
    const auto sensors = awake[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasPowerMetrics);
    EXPECT_FALSE(sensors.hasFanSpeed);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // found: no more re-enumerations

    // Asleep again, the sensor set found while awake is kept, and enumerating doesn't query it.
    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "suspended");
    const unsigned int queriesBefore = controls.deviceQueries();
    const auto again = probe.enumerateGPUs();
    EXPECT_EQ(controls.deviceQueries(), queriesBefore);
    ASSERT_EQ(again.size(), 2U);
    EXPECT_TRUE(again[1].sensorCapabilities.has_value());
}

// #1270: GPUs already asleep when NVML starts get no device-addressed call at all -- not even the
// handle lookup, which is what wakes a GPU -- at load, at enumeration or at a rescan. They are
// described from sysfs and the driver's procfs instead.
TEST(LinuxNVMLGPUProbeTest, GpusAsleepAtLoadAreNotAddressed)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.countsQueriesPerDevice());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_enum_asleep");
    const TestSupport::ScopedTempDir procRoot("tasksmack_nvml_enum_asleep_proc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia", "suspended");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia", "suspended");
    std::ofstream(pciRoot.path / "0000:01:00.0" / "device") << "0x2684\n";
    makeProcGpuInformation(procRoot.path, "0000:01:00.0", "Mock NVIDIA GPU 0", "mock-nvml-uuid-0");
    const unsigned int device0Before = controls.queriesForDevice(0);
    const unsigned int device1Before = controls.queriesForDevice(1);

    NVMLGPUProbe probe(pciRoot.path.string(), procRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    const auto counters = probe.readGPUCounters();
    const auto processes = probe.readProcessGPUCounters();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.queriesForDevice(0), device0Before);
    EXPECT_EQ(controls.queriesForDevice(1), device1Before);

    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].name, "Mock NVIDIA GPU 0"); // from procfs
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");    // the UUID the driver cached: the id NVML will give
    EXPECT_EQ(gpus[0].pciLocation, (PciLocation{.bus = 0x01, .device = 0}));
    EXPECT_EQ(gpus[0].pciDeviceId, 0x2684'10DEU); // NVML's encoding, from sysfs
    EXPECT_FALSE(gpus[0].sensorCapabilities.has_value());
    EXPECT_EQ(gpus[1].name, "NVIDIA GPU"); // no procfs entry: a generic name until it wakes
    EXPECT_EQ(gpus[1].id, "nvidia-0000:41:00.0");
    EXPECT_EQ(gpus[1].pciLocation, (PciLocation{.bus = 0x41, .device = 0}));
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_FALSE(counters[0].utilizationAvailable);
    EXPECT_TRUE(processes.empty());
}

// #1270: a GPU deferred at load is looked up by its PCI address once it wakes: the quick rescan asks
// for a re-enumeration, which reads its NVML identity, index and sensors. The id the driver's procfs
// gave is kept (NVML can't report this mock device's UUID), so its history carries on. The GPU that
// was awake at load was looked up the same way and read as normal throughout.
TEST(LinuxNVMLGPUProbeTest, AGpuDeferredAtLoadIsLookedUpOnceAwake)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.countsQueriesPerDevice());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_deferred_wake");
    const TestSupport::ScopedTempDir procRoot("tasksmack_nvml_deferred_wake_proc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia", "suspended");
    makeProcGpuInformation(procRoot.path, "0000:41:00.0", "Mock NVIDIA GPU 1 (procfs)", "GPU-proc-uuid-1");
    const unsigned int device1Before = controls.queriesForDevice(1);

    NVMLGPUProbe probe(pciRoot.path.string(), procRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto asleep = probe.enumerateGPUs();
    const auto asleepCounters = probe.readGPUCounters();
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
    EXPECT_EQ(controls.queriesForDevice(1), device1Before);
    ASSERT_EQ(asleep.size(), 2U);
    EXPECT_EQ(asleep[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(asleep[0].name, "Mock NVIDIA GPU 0");
    EXPECT_EQ(asleep[0].deviceIndex, 0U);
    EXPECT_TRUE(asleep[0].sensorCapabilities.has_value());
    EXPECT_EQ(asleep[1].id, "GPU-proc-uuid-1");
    EXPECT_EQ(asleep[1].name, "Mock NVIDIA GPU 1 (procfs)");
    ASSERT_EQ(asleepCounters.size(), 2U);
    EXPECT_DOUBLE_EQ(asleepCounters[0].utilizationPercent, 75.0);
    EXPECT_TRUE(asleepCounters[1].suspended);

    // Awake, but not yet looked up: unread (not marked asleep), and no process query through it.
    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "active");
    const auto beforeLookup = probe.readGPUCounters();
    ASSERT_EQ(beforeLookup.size(), 2U);
    EXPECT_FALSE(beforeLookup[1].suspended);
    EXPECT_FALSE(beforeLookup[1].utilizationAvailable);
    EXPECT_EQ(controls.queriesForDevice(1), device1Before);

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto awake = probe.enumerateGPUs();
    EXPECT_GT(controls.queriesForDevice(1), device1Before);
    ASSERT_EQ(awake.size(), 2U);
    EXPECT_EQ(awake[1].id, "GPU-proc-uuid-1"); // kept: NVML has no UUID for this device
    EXPECT_EQ(awake[1].name, "Mock NVIDIA GPU 1");
    EXPECT_EQ(awake[1].deviceIndex, 1U);
    ASSERT_TRUE(awake[1].sensorCapabilities.has_value());
    EXPECT_FALSE(awake[1].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // nothing left to find

    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[1].gpuId, "GPU-proc-uuid-1");
    EXPECT_FALSE(counters[1].suspended);
    EXPECT_DOUBLE_EQ(counters[1].utilizationPercent, 25.0);
    EXPECT_EQ(counters[1].memoryTotalBytes, 16ULL * 1024ULL * 1024ULL * 1024ULL);
}

// #1270: without a procfs entry the deferred GPU's provisional id is its PCI address; once awake it
// takes NVML's id, as a GPU awake at load would have had.
TEST(LinuxNVMLGPUProbeTest, AGpuDeferredWithoutProcfsTakesNvmlsIdOnceAwake)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_deferred_noproc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia", "suspended");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");

    NVMLGPUProbe probe(pciRoot.path.string(), TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    const auto asleep = probe.enumerateGPUs();
    ASSERT_EQ(asleep.size(), 2U);
    EXPECT_EQ(asleep[0].id, "nvidia-0000:01:00.0");
    EXPECT_EQ(asleep[1].id, "nvidia-1"); // awake at load: looked up by address, NVML's fallback id

    setRuntimeStatus(pciRoot.path, "0000:01:00.0", "active");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto awake = probe.enumerateGPUs();
    ASSERT_EQ(awake.size(), 2U);
    EXPECT_EQ(awake[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(awake[0].name, "Mock NVIDIA GPU 0");
}

// #1270: when sysfs doesn't account for every device NVML counts (WSL has no PCI sysfs; here one of
// two is missing), the devices can't all be found by address, so they are enumerated by index as
// before -- a GPU asleep then is described by NVML, as it always was.
TEST(LinuxNVMLGPUProbeTest, SysfsNotListingEveryNvmlDeviceFallsBackToIndexEnumeration)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_deferred_fallback");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia", "suspended");

    NVMLGPUProbe probe(pciRoot.path.string(), TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    EXPECT_EQ(gpus[1].name, "Mock NVIDIA GPU 1");
}

// #1270: a GPU known while awake and asleep through an NVML restart is deferred by the restart, but
// keeps the identity it had -- found by PCI address -- with its sensor set and memory total, so its
// history carries on and it isn't addressed.
TEST(LinuxNVMLGPUProbeTest, AGpuDeferredByARestartKeepsItsIdentity)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsDeviceSet());
    ASSERT_TRUE(controls.countsQueriesPerDevice());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_deferred_restart");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");

    NVMLGPUProbe probe(pciRoot.path.string(), TestSupport::ISOLATED_PCI_ROOT);
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);   // both sensor sets found while awake
    ASSERT_EQ(probe.readGPUCounters().size(), 2U); // and both memory totals

    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "suspended");
    controls.setLostDevice(0);
    [[maybe_unused]] const auto lost = probe.readGPUCounters(); // device 0 reports itself lost
    controls.setLostDevice(NvmlMockControls::NO_FAILING_HANDLE);
    const unsigned int device1Before = controls.queriesForDevice(1);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)); // re-initialises NVML
    const auto gpus = probe.enumerateGPUs();
    const auto counters = probe.readGPUCounters();
    EXPECT_EQ(controls.queriesForDevice(1), device1Before);

    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    EXPECT_EQ(gpus[1].name, "Mock NVIDIA GPU 1");
    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    EXPECT_FALSE(gpus[1].sensorCapabilities.value_or(GPUCapabilities{}).hasPowerMetrics);
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_EQ(counters[1].memoryTotalBytes, 16ULL * 1024ULL * 1024ULL * 1024ULL);

    // Awake again: looked up, and the id stays.
    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "active");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto awake = probe.enumerateGPUs();
    ASSERT_EQ(awake.size(), 2U);
    EXPECT_EQ(awake[1].id, "nvidia-1");
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick));
}

// #1270 review: a UUID reported at a restart is authoritative. A GPU asleep through the restart whose
// procfs entry now gives a different UUID from the one known at its PCI address is a different GPU in
// that slot: it keeps its own id and gets none of the old GPU's sensor set or memory total.
TEST(LinuxNVMLGPUProbeTest, ADifferentGpuAtAKnownAddressAfterARestartGetsNothingOfTheOldOnes)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsDeviceSet());
    ASSERT_TRUE(controls.countsQueriesPerDevice());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_restart_replaced");
    const TestSupport::ScopedTempDir procRoot("tasksmack_nvml_restart_replaced_proc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");

    NVMLGPUProbe probe(pciRoot.path.string(), procRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    const auto before = probe.enumerateGPUs();
    ASSERT_EQ(before.size(), 2U);
    ASSERT_EQ(before[0].id, "mock-nvml-uuid-0");
    ASSERT_EQ(probe.readGPUCounters().size(), 2U); // device 0's memory total is known

    // Device 0's slot now holds another GPU, asleep, whose UUID the driver's procfs reports.
    setRuntimeStatus(pciRoot.path, "0000:01:00.0", "suspended");
    makeProcGpuInformation(procRoot.path, "0000:01:00.0", "Replacement GPU", "GPU-replacement-uuid");
    controls.setLostDevice(1);
    [[maybe_unused]] const auto lost = probe.readGPUCounters(); // device 1 reports itself lost
    controls.setLostDevice(NvmlMockControls::NO_FAILING_HANDLE);
    const unsigned int device0Before = controls.queriesForDevice(0);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)); // re-initialises NVML
    const auto gpus = probe.enumerateGPUs();
    const auto counters = probe.readGPUCounters();
    EXPECT_EQ(controls.queriesForDevice(0), device0Before); // asleep: not addressed

    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "GPU-replacement-uuid");
    EXPECT_EQ(gpus[0].name, "Replacement GPU");
    EXPECT_FALSE(gpus[0].sensorCapabilities.has_value()); // the old GPU's sensor set isn't its
    EXPECT_EQ(gpus[1].id, "nvidia-1");                    // the GPU in the other slot is unchanged
    EXPECT_TRUE(gpus[1].sensorCapabilities.has_value());
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[0].gpuId, "GPU-replacement-uuid");
    EXPECT_TRUE(counters[0].suspended);
    EXPECT_EQ(counters[0].memoryTotalBytes, 0U); // nor is its memory total
}

// #1270 review: a GPU awake at a restart whose UUID NVML can't report this time keeps the UUID known
// for its PCI address -- the one NVML gave earlier, or the driver's procfs gave while it was deferred
// -- with its sensor set, rather than an index-based "nvidia-N" id that would break its history.
TEST(LinuxNVMLGPUProbeTest, AnAwakeGpuWithoutAUuidAtARestartKeepsItsRememberedUuid)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.available());
    ASSERT_TRUE(controls.controlsDeviceSet());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_restart_no_uuid");
    const TestSupport::ScopedTempDir procRoot("tasksmack_nvml_restart_no_uuid_proc");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia", "suspended");
    makeProcGpuInformation(procRoot.path, "0000:41:00.0", "Mock NVIDIA GPU 1 (procfs)", "GPU-proc-uuid-1");

    NVMLGPUProbe probe(pciRoot.path.string(), procRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);
    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "active");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Quick));
    const auto before = probe.enumerateGPUs(); // device 1 looked up; NVML has no UUID for it
    ASSERT_EQ(before.size(), 2U);
    ASSERT_EQ(before[0].id, "mock-nvml-uuid-0");
    ASSERT_EQ(before[1].id, "GPU-proc-uuid-1");
    ASSERT_TRUE(before[1].sensorCapabilities.has_value());

    // Restart with both awake (so enumerated by index), NVML's UUID query now failing for device 0 too.
    controls.configure(NvmlMockControls::NO_FAILING_HANDLE, 0);
    controls.setLostDevice(0);
    [[maybe_unused]] const auto lost = probe.readGPUCounters();
    controls.setLostDevice(NvmlMockControls::NO_FAILING_HANDLE);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_GT(controls.uuidCalls(), 0U); // the rebuilt devices were asked, and failed

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");
    EXPECT_EQ(gpus[1].id, "GPU-proc-uuid-1");
    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    EXPECT_FALSE(gpus[1].sensorCapabilities.value_or(GPUCapabilities{}).hasFanSpeed);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_EQ(counters[0].gpuId, "mock-nvml-uuid-0");
    EXPECT_EQ(counters[1].gpuId, "GPU-proc-uuid-1");
    EXPECT_DOUBLE_EQ(counters[1].utilizationPercent, 25.0);
}

// #1270 review: whether a device rebuilt by a restart takes the id remembered for its PCI address.
TEST(NVMLGPUProbeMathTest, ARestartKeepsTheRememberedIdOnlyWhenNoUuidIsReported)
{
    // A UUID reported now stands, whatever was remembered.
    EXPECT_FALSE(NVMLGPUProbeMath::keepsRememberedId(true, true, true));
    EXPECT_FALSE(NVMLGPUProbeMath::keepsRememberedId(true, false, true));
    EXPECT_FALSE(NVMLGPUProbeMath::keepsRememberedId(true, false, false));
    // None reported: a remembered UUID is reused, awake (a failed UUID query) or deferred.
    EXPECT_TRUE(NVMLGPUProbeMath::keepsRememberedId(false, true, true));
    EXPECT_TRUE(NVMLGPUProbeMath::keepsRememberedId(false, false, true));
    // Neither has a UUID: a deferred device keeps its remembered fallback id; an awake one has NVML's.
    EXPECT_TRUE(NVMLGPUProbeMath::keepsRememberedId(false, false, false));
    EXPECT_FALSE(NVMLGPUProbeMath::keepsRememberedId(false, true, false));
}

TEST(NVMLGPUProbeMathTest, ParsesSysfsAndNvmlPciAddresses)
{
    const auto sysfs = NVMLGPUProbeMath::parsePciAddress("0000:41:1f.3");
    ASSERT_TRUE(sysfs.has_value());
    EXPECT_EQ(sysfs.value_or(NVMLGPUProbeMath::PciAddressFields{}).bus, 0x41U);
    EXPECT_EQ(sysfs.value_or(NVMLGPUProbeMath::PciAddressFields{}).device, 0x1fU);
    EXPECT_EQ(sysfs.value_or(NVMLGPUProbeMath::PciAddressFields{}).function, 3U);
    const auto nvml = NVMLGPUProbeMath::parsePciAddress("00010000:C1:00.0");
    ASSERT_TRUE(nvml.has_value());
    EXPECT_EQ(nvml.value_or(NVMLGPUProbeMath::PciAddressFields{}).domain, 0x10000U);
    EXPECT_EQ(nvml.value_or(NVMLGPUProbeMath::PciAddressFields{}).bus, 0xc1U);

    for (const char* bad : {"", "0000", "0000:01", "0000:01:00", "0000:01.00:0", "0000:zz:00.0", "0000:01:00.", ":01:00.0"})
    {
        EXPECT_FALSE(NVMLGPUProbeMath::parsePciAddress(bad).has_value()) << bad;
    }
}

TEST(NVMLGPUProbeMathTest, ParsesTheDriversGpuInformation)
{
    const auto info = NVMLGPUProbeMath::parseNvidiaProcGpuInformation("Model: \t\t NVIDIA GeForce RTX 4090 Laptop GPU\n"
                                                                      "IRQ:   \t\t 142\n"
                                                                      "GPU UUID: \t GPU-3f1c2a6e-0d4b-11ee-be56-0242ac120002\n"
                                                                      "Video BIOS: \t 95.03.1d.00.80\n");
    EXPECT_EQ(info.model, "NVIDIA GeForce RTX 4090 Laptop GPU");
    EXPECT_EQ(info.uuid, "GPU-3f1c2a6e-0d4b-11ee-be56-0242ac120002");

    // A UUID the driver hasn't cached yet is printed with placeholders: no UUID.
    // (Built up, since "??-" in a literal is a trigraph.)
    const std::string placeholder = "GPU-" + std::string(8, '?') + "-" + std::string(4, '?') + "-" + std::string(12, '?');
    const auto unknown = NVMLGPUProbeMath::parseNvidiaProcGpuInformation("Model: \t\t NVIDIA T500\r\nGPU UUID: \t " + placeholder + "\r\n");
    EXPECT_EQ(unknown.model, "NVIDIA T500");
    EXPECT_TRUE(unknown.uuid.empty());

    const auto empty = NVMLGPUProbeMath::parseNvidiaProcGpuInformation("");
    EXPECT_TRUE(empty.model.empty());
    EXPECT_TRUE(empty.uuid.empty());
}

TEST(PciDisplayDevicesTest, AddressesBoundToADriver)
{
    const std::vector<std::string> devices{"0000:01:00.0=nvidia", "0000:41:00.0=", "0000:42:00.0=nvidia", "0000:43:00.0=nouveau"};
    EXPECT_EQ(PciDisplayDevices::addressesBoundTo(devices, PciDisplayDevices::DRIVER_NVIDIA),
              (std::vector<std::string>{"0000:01:00.0", "0000:42:00.0"}));
    EXPECT_TRUE(PciDisplayDevices::addressesBoundTo({}, PciDisplayDevices::DRIVER_NVIDIA).empty());
}

// #1295 review: a GPU asleep through an NVML restart -- here one triggered by another NVIDIA GPU
// appearing on the bus (the mock NVML still lists two) -- keeps the sensor set found while it was
// awake, and its memory total. It isn't woken to find them again, so forgetting them would republish
// the probe-wide capabilities for it until it woke.
TEST(LinuxNVMLGPUProbeTest, AGpuAsleepThroughARestartKeepsItsSensors)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsDeviceSet());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_restart_asleep");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");

    NVMLGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);   // both sensor sets found while awake
    ASSERT_EQ(probe.readGPUCounters().size(), 2U); // and both memory totals

    setRuntimeStatus(pciRoot.path, "0000:41:00.0", "suspended");
    const unsigned int initsBefore = controls.initCalls();
    makePciDevice(pciRoot.path, "0000:42:00.0", "0x10de", "nvidia");
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[1].id, "nvidia-1");
    ASSERT_TRUE(gpus[1].sensorCapabilities.has_value());
    const auto sensors = gpus[1].sensorCapabilities.value_or(GPUCapabilities{});
    EXPECT_TRUE(sensors.hasTemperature);
    EXPECT_TRUE(sensors.hasClockSpeeds);
    EXPECT_FALSE(sensors.hasPowerMetrics);
    EXPECT_FALSE(sensors.hasFanSpeed);
    const auto counters = probe.readGPUCounters();
    ASSERT_EQ(counters.size(), 2U);
    EXPECT_TRUE(counters[1].suspended);
    EXPECT_EQ(counters[1].memoryTotalBytes, 16ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // nothing left to find
}

// #1295 review: a restart that fails while the NVIDIA GPUs are still there reports no change, so
// GPUModel keeps the known GPU list (instead of publishing an empty one) and the next full rescan
// retries. The restart that works then reports the change.
TEST(LinuxNVMLGPUProbeTest, AFailedRestartWithTheGpusStillPresentReportsNoChange)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsInit());
    ASSERT_TRUE(controls.controlsDeviceSet());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_failed_restart");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");

    NVMLGPUProbe probe(pciRoot.path.string());
    ASSERT_TRUE(probe.isAvailable());
    ASSERT_EQ(probe.enumerateGPUs().size(), 2U);

    controls.setLostDevice(1);
    [[maybe_unused]] const auto lost = probe.readGPUCounters(); // reports the GPU lost
    controls.failInits(1);
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // restart failed, GPUs still bound: keep the list
    EXPECT_FALSE(probe.isAvailable());

    controls.setLostDevice(NvmlMockControls::NO_FAILING_HANDLE);
    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full)); // retried and started: publish
    EXPECT_TRUE(probe.isAvailable());
    EXPECT_EQ(probe.enumerateGPUs().size(), 2U);
}

// #1295 review: NVML failing to start while an nvidia-bound GPU is present (TaskSmack started during
// a driver reload) is retried at each full rescan -- not every sample -- until it starts; the rescan
// that brings it up reports a change, so the GPUs are enumerated.
TEST(LinuxNVMLGPUProbeTest, NvmlNotStartingWithAnNvidiaGpuPresentIsRetriedAtFullRescans)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsInit());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_start_retry");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nvidia");
    makePciDevice(pciRoot.path, "0000:41:00.0", "0x10de", "nvidia");
    controls.failInits(2);
    const unsigned int initsBefore = controls.initCalls();

    NVMLGPUProbe probe(pciRoot.path.string());
    EXPECT_FALSE(probe.isAvailable());
    EXPECT_TRUE(probe.enumerateGPUs().empty());
    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Quick)); // waits for the full rescan
    EXPECT_EQ(controls.initCalls(), initsBefore + 1);

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // still not ready: no change reported, nothing to publish
    EXPECT_EQ(controls.initCalls(), initsBefore + 2);
    EXPECT_FALSE(probe.isAvailable());

    EXPECT_TRUE(probe.rescanGPUs(GPURescan::Full));
    EXPECT_EQ(controls.initCalls(), initsBefore + 3);
    ASSERT_TRUE(probe.isAvailable());
    const auto gpus = probe.enumerateGPUs();
    ASSERT_EQ(gpus.size(), 2U);
    EXPECT_EQ(gpus[0].id, "mock-nvml-uuid-0");

    EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)); // started: no further retries
    EXPECT_EQ(controls.initCalls(), initsBefore + 3);
}

// #1295 review: with no GPU bound to the nvidia driver (none at all, or one on nouveau) NVML failing
// to start is not retried: it could never succeed, and a driver binding later changes the PCI list,
// which restarts NVML anyway.
TEST(LinuxNVMLGPUProbeTest, NvmlNotStartingWithoutAnNvidiaBoundGpuIsNotRetried)
{
    const auto envGuard = TestSupport::checkMockGpuLibrariesPreloaded();
    if (!envGuard.mocksPreloaded())
    {
        GTEST_SKIP() << "Mock NVML library not preloaded; run via CTest or set LD_LIBRARY_PATH=" TASKSMACK_TEST_GPU_MOCK_DIR;
    }
    const NvmlMockControls controls;
    ASSERT_TRUE(controls.controlsInit());
    const TestSupport::ScopedTempDir pciRoot("tasksmack_nvml_start_no_retry");
    makePciDevice(pciRoot.path, "0000:01:00.0", "0x10de", "nouveau");
    makePciDevice(pciRoot.path, "0000:00:02.0", "0x8086", "nvidia"); // not an NVIDIA device

    for (const auto& root : {pciRoot.path.string(), std::string(TestSupport::ISOLATED_PCI_ROOT)})
    {
        controls.failInits(1);
        const unsigned int initsBefore = controls.initCalls();
        NVMLGPUProbe probe(root);
        EXPECT_FALSE(probe.isAvailable()) << root;
        EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << root;
        EXPECT_FALSE(probe.rescanGPUs(GPURescan::Full)) << root;
        EXPECT_EQ(controls.initCalls(), initsBefore + 1) << root;
        EXPECT_FALSE(probe.isAvailable()) << root;
    }
}

} // namespace
} // namespace Platform

#endif
