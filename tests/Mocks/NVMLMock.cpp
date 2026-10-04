#include "Platform/NVMLTypes.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

namespace
{

namespace NVML = Platform::NVML;

struct MockDevice
{
    const char* name;
    const char* uuid;
    bool hasUuid;
    NVML::nvmlMemory_t memory;
    unsigned int utilizationPercent;
    unsigned int temperatureC;
    unsigned int powerMilliwatts;
    unsigned int powerLimitMilliwatts;
    unsigned int graphicsClockMHz;
    unsigned int memoryClockMHz;
    unsigned int fanPercent;
    unsigned int pcieTxKilobytes;
    unsigned int pcieRxKilobytes;
};

constexpr std::array<MockDevice, 2> MOCK_DEVICES{{
    {.name = "Mock NVIDIA GPU 0",
     .uuid = "mock-nvml-uuid-0",
     .hasUuid = true,
     .memory = {.total = 8ULL * 1024ULL * 1024ULL * 1024ULL,
                .free = 6ULL * 1024ULL * 1024ULL * 1024ULL,
                .used = 2ULL * 1024ULL * 1024ULL * 1024ULL},
     .utilizationPercent = 75,
     .temperatureC = 65,
     .powerMilliwatts = 125000,
     .powerLimitMilliwatts = 250000,
     .graphicsClockMHz = 1800,
     .memoryClockMHz = 9000,
     .fanPercent = 40,
     .pcieTxKilobytes = 32,
     .pcieRxKilobytes = 64},
    {.name = "Mock NVIDIA GPU 1",
     .uuid = "",
     .hasUuid = false,
     .memory = {.total = 16ULL * 1024ULL * 1024ULL * 1024ULL,
                .free = 10ULL * 1024ULL * 1024ULL * 1024ULL,
                .used = 6ULL * 1024ULL * 1024ULL * 1024ULL},
     .utilizationPercent = 25,
     .temperatureC = 55,
     .powerMilliwatts = 75000,
     .powerLimitMilliwatts = 200000,
     .graphicsClockMHz = 1500,
     .memoryClockMHz = 7000,
     .fanPercent = 25,
     .pcieTxKilobytes = 8,
     .pcieRxKilobytes = 16},
}};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - mutable handles needed so functions can return stable pointers-to-element as nvmlDevice_t
std::array<int, MOCK_DEVICES.size()> MOCK_HANDLES{1, 2};

// Sentinel returned by deviceIndex() when the handle is not found
constexpr std::size_t INVALID_DEVICE_INDEX = std::numeric_limits<std::size_t>::max();

[[nodiscard]] auto deviceIndex(NVML::nvmlDevice_t device) -> std::size_t
{
    for (std::size_t i = 0; i < MOCK_HANDLES.size(); ++i)
    {
        if (device == &MOCK_HANDLES[i])
        {
            return i;
        }
    }

    // Return sentinel — callers must check before indexing MOCK_DEVICES
    return INVALID_DEVICE_INDEX;
}

// Returns a pointer to the mock device for the given handle, or nullptr for invalid handles.
[[nodiscard]] auto safeDevice(NVML::nvmlDevice_t device) -> const MockDevice*
{
    const auto idx = deviceIndex(device);
    if (idx == INVALID_DEVICE_INDEX || idx >= MOCK_DEVICES.size())
    {
        return nullptr;
    }
    return &MOCK_DEVICES[idx];
}

void writeString(const char* source, char* destination, unsigned int length)
{
    if (length == 0)
    {
        return;
    }

    std::strncpy(destination, source, length);
    destination[length - 1] = '\0';
}

// Running processes, as real NVML reports them (#1092):
//   - a count-only call (null buffer) returns NVML_ERROR_INSUFFICIENT_SIZE and the needed count
//     whenever any process is running, and NVML_SUCCESS only when none are;
//   - a buffer smaller than the list also gets NVML_ERROR_INSUFFICIENT_SIZE;
//   - the unversioned symbols write the 16-byte nvmlProcessInfo_v1_t, the _v3 symbols the
//     24-byte nvmlProcessInfo_v2_t;
//   - usedGpuMemory is NVML_VALUE_NOT_AVAILABLE (ULLONG_MAX) when it can't be read.
// The probe must not share these layouts with the mock, or the mock can't catch a mismatch.

// NOLINTNEXTLINE(readability-identifier-naming) - mirrors NVML's nvmlProcessInfo_v1_t
struct ProcessInfoV1
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
};
static_assert(sizeof(ProcessInfoV1) == 16);

// NOLINTNEXTLINE(readability-identifier-naming) - mirrors NVML's nvmlProcessInfo_v2_t
struct ProcessInfoV2
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
};
static_assert(sizeof(ProcessInfoV2) == 24);

struct MockProcess
{
    unsigned int pid;
    unsigned long long usedGpuMemory;
};

constexpr unsigned long long VALUE_NOT_AVAILABLE = std::numeric_limits<unsigned long long>::max();
constexpr std::array<MockProcess, 1> COMPUTE_PROCESSES{{{.pid = 123U, .usedGpuMemory = 111ULL}}};
constexpr std::array<MockProcess, 3> GRAPHICS_PROCESSES{{
    {.pid = 123U, .usedGpuMemory = 222ULL},
    {.pid = 456U, .usedGpuMemory = 333ULL},
    {.pid = 789U, .usedGpuMemory = VALUE_NOT_AVAILABLE},
}};

template<typename Info, std::size_t N>
NVML::nvmlReturn_t listProcesses(NVML::nvmlDevice_t device, unsigned int* count, void* infos, const std::array<MockProcess, N>& processes)
{
    const auto idx = deviceIndex(device);
    if (idx == INVALID_DEVICE_INDEX || idx != 0)
    {
        *count = 0;
        return NVML::NVML_SUCCESS;
    }

    const unsigned int capacity = *count;
    *count = static_cast<unsigned int>(N);
    if (infos == nullptr || capacity < N)
    {
        return NVML::NVML_ERROR_INSUFFICIENT_SIZE;
    }

    auto* out = static_cast<Info*>(infos);
    for (std::size_t i = 0; i < N; ++i)
    {
        out[i] = Info{};
        out[i].pid = processes[i].pid;
        out[i].usedGpuMemory = processes[i].usedGpuMemory;
    }
    return NVML::NVML_SUCCESS;
}

// Test controls (#1162), set through the tasksmackNvmlMock* functions below. Each test runs in
// its own process under CTest, but tests reset them anyway.
constexpr unsigned int NO_FAILING_HANDLE = std::numeric_limits<unsigned int>::max();
unsigned int g_FailingHandleIndex = NO_FAILING_HANDLE;
int g_UuidCallsBeforeFailure = -1; // -1: never fail
unsigned int g_UuidCalls = 0;
bool g_FailSensorReads = false; // utilization, temperature, power and graphics clock time out (#1111)

} // namespace

extern "C"
{

    NVML::nvmlReturn_t nvmlInit_v2()
    {
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlShutdown()
    {
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* count)
    {
        *count = static_cast<unsigned int>(MOCK_DEVICES.size());
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, NVML::nvmlDevice_t* device)
    {
        if (index >= MOCK_HANDLES.size())
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (index == g_FailingHandleIndex)
        {
            return NVML::NVML_ERROR_UNKNOWN;
        }
        *device = &MOCK_HANDLES[index];
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetName(NVML::nvmlDevice_t device, char* name, unsigned int length)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        writeString(dev->name, name, length);
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetUUID(NVML::nvmlDevice_t device, char* uuid, unsigned int length)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        ++g_UuidCalls;
        if (g_UuidCallsBeforeFailure >= 0 && std::cmp_greater(g_UuidCalls, g_UuidCallsBeforeFailure))
        {
            return NVML::NVML_ERROR_UNKNOWN;
        }
        if (!dev->hasUuid)
        {
            return NVML::NVML_ERROR_NOT_FOUND;
        }

        writeString(dev->uuid, uuid, length);
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetMemoryInfo(NVML::nvmlDevice_t device, NVML::nvmlMemory_t* memory)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        *memory = dev->memory;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetUtilizationRates(NVML::nvmlDevice_t device, NVML::nvmlUtilization_t* utilization)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (g_FailSensorReads)
        {
            return NVML::NVML_ERROR_TIMEOUT;
        }
        utilization->gpu = dev->utilizationPercent;
        utilization->memory = 0;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t
    nvmlDeviceGetTemperature(NVML::nvmlDevice_t device, NVML::nvmlTemperatureSensors_t /*sensor*/, unsigned int* temperature)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (g_FailSensorReads)
        {
            return NVML::NVML_ERROR_TIMEOUT;
        }
        *temperature = dev->temperatureC;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetPowerUsage(NVML::nvmlDevice_t device, unsigned int* power)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (g_FailSensorReads)
        {
            return NVML::NVML_ERROR_TIMEOUT;
        }
        *power = dev->powerMilliwatts;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetPowerManagementLimit(NVML::nvmlDevice_t device, unsigned int* limit)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        *limit = dev->powerLimitMilliwatts;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetClockInfo(NVML::nvmlDevice_t device, NVML::nvmlClockType_t type, unsigned int* clock)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (g_FailSensorReads && type != NVML::NVML_CLOCK_MEM)
        {
            return NVML::NVML_ERROR_TIMEOUT;
        }
        *clock = (type == NVML::NVML_CLOCK_MEM) ? dev->memoryClockMHz : dev->graphicsClockMHz;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetFanSpeed(NVML::nvmlDevice_t device, unsigned int* fanSpeed)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        *fanSpeed = dev->fanPercent;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetPcieThroughput(NVML::nvmlDevice_t device, NVML::nvmlPcieUtilCounter_t counter, unsigned int* throughput)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        *throughput = (counter == NVML::NVML_PCIE_UTIL_TX_BYTES) ? dev->pcieTxKilobytes : dev->pcieRxKilobytes;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, void* infos)
    {
        return listProcesses<ProcessInfoV1>(device, count, infos, COMPUTE_PROCESSES);
    }

    NVML::nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, void* infos)
    {
        return listProcesses<ProcessInfoV1>(device, count, infos, GRAPHICS_PROCESSES);
    }

    NVML::nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(NVML::nvmlDevice_t device, unsigned int* count, void* infos)
    {
        return listProcesses<ProcessInfoV2>(device, count, infos, COMPUTE_PROCESSES);
    }

    NVML::nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(NVML::nvmlDevice_t device, unsigned int* count, void* infos)
    {
        return listProcesses<ProcessInfoV2>(device, count, infos, GRAPHICS_PROCESSES);
    }

    // Test controls (not part of NVML). failingHandleIndex: nvmlDeviceGetHandleByIndex_v2 fails for
    // that index (NO_FAILING_HANDLE for none). uuidCallsBeforeFailure: nvmlDeviceGetUUID fails
    // once it has been called more than this many times (-1 for never). Also resets the counter.
    void tasksmackNvmlMockConfigure(unsigned int failingHandleIndex, int uuidCallsBeforeFailure)
    {
        g_FailingHandleIndex = failingHandleIndex;
        g_UuidCallsBeforeFailure = uuidCallsBeforeFailure;
        g_UuidCalls = 0;
    }

    // Test control: make the utilization, temperature, power and graphics-clock reads fail with
    // NVML_ERROR_TIMEOUT, as a busy or resetting GPU does (#1111).
    void tasksmackNvmlMockFailSensorReads(int fail)
    {
        g_FailSensorReads = (fail != 0);
    }

    unsigned int tasksmackNvmlMockUuidCalls()
    {
        return g_UuidCalls;
    }

    const char* nvmlErrorString(NVML::nvmlReturn_t /*result*/)
    {
        return "Mock NVML error";
    }

} // extern "C"
