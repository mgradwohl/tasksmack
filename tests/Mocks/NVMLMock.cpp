#include "Platform/NVMLTypes.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
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
    // A laptop GPU may report no power and a passively cooled one no fan: those reads then return
    // NVML_ERROR_NOT_SUPPORTED, so per-device sensor capabilities can be tested (#1112).
    bool hasPower;
    bool hasFan;
    // A GPU without NVENC/NVDEC answers the video-engine queries NVML_ERROR_NOT_SUPPORTED (#1477).
    bool hasVideoEngines;
    unsigned int encoderPercent;
    unsigned int decoderPercent;
    const char* busId; // nvmlPciInfo_t::busId, eight-digit domain as NVML prints it (#1117)
    unsigned int pciBus;
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
     .pcieRxKilobytes = 64,
     .hasPower = true,
     .hasFan = true,
     .hasVideoEngines = true,
     .encoderPercent = 30,
     .decoderPercent = 12,
     .busId = "00000000:01:00.0",
     .pciBus = 0x01},
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
     .pcieRxKilobytes = 16,
     .hasPower = false,
     .hasFan = false,
     .hasVideoEngines = false,
     .encoderPercent = 0,
     .decoderPercent = 0,
     .busId = "00000000:41:00.0",
     .pciBus = 0x41},
}};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - mutable handles needed so functions can return stable pointers-to-element as nvmlDevice_t
std::array<int, MOCK_DEVICES.size()> MOCK_HANDLES{1, 2};

// Sentinel returned by deviceIndex() when the handle is not found
constexpr std::size_t INVALID_DEVICE_INDEX = std::numeric_limits<std::size_t>::max();

// Every call that addresses a device (any per-device query, the process lists included), so a test
// can prove a runtime-suspended GPU wasn't touched (#1117). Read via tasksmackNvmlMockDeviceQueries().
unsigned int g_DeviceQueries = 0; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - mock call counter

// Per mock device, every call that addressed it, handle lookups (by index or PCI bus id) included:
// getting a handle is what makes real NVML initialise -- and so wake -- a GPU (#1270). Read via
// tasksmackNvmlMockQueriesForDevice().
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - mock call counters
std::array<unsigned int, MOCK_DEVICES.size()> g_QueriesPerDevice{};

[[nodiscard]] auto deviceIndex(NVML::nvmlDevice_t device) -> std::size_t
{
    ++g_DeviceQueries;
    for (std::size_t i = 0; i < MOCK_HANDLES.size(); ++i)
    {
        if (device == &MOCK_HANDLES[i])
        {
            ++g_QueriesPerDevice.at(i);
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
bool g_FailSensorReads = false; // utilization, memory, temperature, power, graphics clock, encoder and decoder time out (#1111)
// #1116: how many devices NVML reports (the first N of MOCK_DEVICES), so a test can hot-plug or remove
// one; which device's sensor reads return NVML_ERROR_GPU_IS_LOST; and how often NVML was initialised.
unsigned int g_DeviceCount = static_cast<unsigned int>(MOCK_DEVICES.size());
unsigned int g_LostDeviceIndex = NO_FAILING_HANDLE;
unsigned int g_InitCalls = 0;
// How many of the next nvmlInit_v2 calls fail with NVML_ERROR_DRIVER_NOT_LOADED, as during a driver
// reload (#1116). Set through tasksmackNvmlMockFailInits().
unsigned int g_FailingInits = 0;
// #1270 review: while set, mock device 0 presents as the GPU that held its slot before a replacement --
// another UUID, and no power reading -- so a test can tell that GPU's remembered state from the
// replacement's own. Set through tasksmackNvmlMockSetPreviousOccupant().
bool g_PreviousOccupant = false; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - mock test control
constexpr const char* PREVIOUS_OCCUPANT_UUID = "mock-nvml-previous-uuid";
// #1353: which mock device's nvmlDeviceGetPciInfo_v3 fails (NO_FAILING_HANDLE: none). Set through
// tasksmackNvmlMockSetFailingPciInfoDevice().
unsigned int g_FailingPciInfoIndex = NO_FAILING_HANDLE; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables) - mock test control

/// Whether `dev` currently presents as the previous occupant of device 0's slot.
[[nodiscard]] bool isPreviousOccupant(const MockDevice* dev)
{
    return g_PreviousOccupant && dev == MOCK_DEVICES.data();
}

/// Whether `dev` is the device the test marked lost (#1116). Doesn't count as a device query.
[[nodiscard]] bool isLost(const MockDevice* dev)
{
    return g_LostDeviceIndex < MOCK_DEVICES.size() && dev == &MOCK_DEVICES[g_LostDeviceIndex];
}

// The video engines' utilization (#1477), as real NVML reports it: the percentage and the period
// it was averaged over.
NVML::nvmlReturn_t
videoEngineUtilization(NVML::nvmlDevice_t device, bool encoder, unsigned int* utilization, unsigned int* samplingPeriodUs)
{
    const auto* dev = safeDevice(device);
    if (dev == nullptr)
    {
        return NVML::NVML_ERROR_INVALID_ARGUMENT;
    }
    if (isLost(dev))
    {
        return NVML::NVML_ERROR_GPU_IS_LOST;
    }
    if (!dev->hasVideoEngines)
    {
        return NVML::NVML_ERROR_NOT_SUPPORTED;
    }
    if (g_FailSensorReads)
    {
        return NVML::NVML_ERROR_TIMEOUT;
    }
    constexpr unsigned int SAMPLING_PERIOD_US = 167'000;
    *utilization = encoder ? dev->encoderPercent : dev->decoderPercent;
    *samplingPeriodUs = SAMPLING_PERIOD_US;
    return NVML::NVML_SUCCESS;
}

} // namespace

extern "C"
{

    NVML::nvmlReturn_t nvmlInit_v2()
    {
        ++g_InitCalls;
        if (g_FailingInits > 0)
        {
            --g_FailingInits;
            return NVML::NVML_ERROR_DRIVER_NOT_LOADED;
        }
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlShutdown()
    {
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int* count)
    {
        *count = g_DeviceCount;
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, NVML::nvmlDevice_t* device)
    {
        if (index >= MOCK_HANDLES.size() || index >= g_DeviceCount)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        ++g_QueriesPerDevice.at(index);
        if (index == g_FailingHandleIndex)
        {
            return NVML::NVML_ERROR_UNKNOWN;
        }
        *device = &MOCK_HANDLES[index];
        return NVML::NVML_SUCCESS;
    }

    // Matches "bus:device.function" after the domain, so the kernel's four-digit domain ("0000:01:00.0")
    // and NVML's eight-digit one ("00000000:01:00.0") both find a device, as with real NVML.
    // NOLINTNEXTLINE(readability-identifier-naming) - the exported NVML symbol name
    NVML::nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char* pciBusId, NVML::nvmlDevice_t* device)
    {
        if (pciBusId == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        const std::string_view wanted(pciBusId);
        const auto afterDomain = [](std::string_view busId)
        {
            return busId.substr(busId.find(':') + 1);
        };
        for (unsigned int index = 0; index < g_DeviceCount && index < MOCK_DEVICES.size(); ++index)
        {
            if (wanted.contains(':') && afterDomain(wanted) == afterDomain(MOCK_DEVICES.at(index).busId))
            {
                ++g_QueriesPerDevice.at(index);
                if (index == g_FailingHandleIndex)
                {
                    return NVML::NVML_ERROR_UNKNOWN;
                }
                *device = &MOCK_HANDLES.at(index);
                return NVML::NVML_SUCCESS;
            }
        }
        return NVML::NVML_ERROR_NOT_FOUND;
    }

    NVML::nvmlReturn_t nvmlDeviceGetIndex(NVML::nvmlDevice_t device, unsigned int* index)
    {
        const auto idx = deviceIndex(device);
        if (idx == INVALID_DEVICE_INDEX)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        *index = static_cast<unsigned int>(idx);
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
        if (isPreviousOccupant(dev))
        {
            writeString(PREVIOUS_OCCUPANT_UUID, uuid, length);
            return NVML::NVML_SUCCESS;
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
        if (isLost(dev))
        {
            return NVML::NVML_ERROR_GPU_IS_LOST;
        }
        if (g_FailSensorReads)
        {
            return NVML::NVML_ERROR_TIMEOUT;
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
        if (isLost(dev))
        {
            return NVML::NVML_ERROR_GPU_IS_LOST;
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
        if (isLost(dev))
        {
            return NVML::NVML_ERROR_GPU_IS_LOST;
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
        if (isLost(dev))
        {
            return NVML::NVML_ERROR_GPU_IS_LOST;
        }
        if (!dev->hasPower || isPreviousOccupant(dev))
        {
            return NVML::NVML_ERROR_NOT_SUPPORTED;
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
        if (isLost(dev))
        {
            return NVML::NVML_ERROR_GPU_IS_LOST;
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
        if (!dev->hasFan)
        {
            return NVML::NVML_ERROR_NOT_SUPPORTED;
        }
        *fanSpeed = dev->fanPercent;
        return NVML::NVML_SUCCESS;
    }

    // NOLINTNEXTLINE(readability-identifier-naming) - the exported NVML symbol name
    NVML::nvmlReturn_t nvmlDeviceGetPciInfo_v3(NVML::nvmlDevice_t device, NVML::nvmlPciInfo_t* pci)
    {
        const auto* dev = safeDevice(device);
        if (dev == nullptr)
        {
            return NVML::NVML_ERROR_INVALID_ARGUMENT;
        }
        if (g_FailingPciInfoIndex < MOCK_DEVICES.size() && dev == &MOCK_DEVICES[g_FailingPciInfoIndex])
        {
            return NVML::NVML_ERROR_UNKNOWN;
        }
        *pci = NVML::nvmlPciInfo_t{};
        pci->domain = 0;
        pci->bus = dev->pciBus;
        pci->device = 0;
        pci->pciDeviceId = 0x2684'10DEU;
        writeString(dev->busId, std::data(pci->busId), static_cast<unsigned int>(std::size(pci->busId)));
        writeString(dev->busId, std::data(pci->busIdLegacy), static_cast<unsigned int>(std::size(pci->busIdLegacy)));
        return NVML::NVML_SUCCESS;
    }

    NVML::nvmlReturn_t nvmlDeviceGetEncoderUtilization(NVML::nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs)
    {
        return videoEngineUtilization(device, true, utilization, samplingPeriodUs);
    }

    NVML::nvmlReturn_t nvmlDeviceGetDecoderUtilization(NVML::nvmlDevice_t device, unsigned int* utilization, unsigned int* samplingPeriodUs)
    {
        return videoEngineUtilization(device, false, utilization, samplingPeriodUs);
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

    NVML::nvmlReturn_t
    nvmlDeviceGetComputeRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfoEntries* infos)
    {
        return listProcesses<ProcessInfoV1>(device, count, infos, COMPUTE_PROCESSES);
    }

    NVML::nvmlReturn_t
    nvmlDeviceGetGraphicsRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfoEntries* infos)
    {
        return listProcesses<ProcessInfoV1>(device, count, infos, GRAPHICS_PROCESSES);
    }

    NVML::nvmlReturn_t
    nvmlDeviceGetComputeRunningProcesses_v3(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfoEntries* infos)
    {
        return listProcesses<ProcessInfoV2>(device, count, infos, COMPUTE_PROCESSES);
    }

    NVML::nvmlReturn_t
    nvmlDeviceGetGraphicsRunningProcesses_v3(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfoEntries* infos)
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

    // Test control: make the utilization, memory, temperature, power, graphics-clock, encoder and decoder reads fail with
    // NVML_ERROR_TIMEOUT, as a busy or resetting GPU does (#1111).
    void tasksmackNvmlMockFailSensorReads(int fail)
    {
        g_FailSensorReads = (fail != 0);
    }

    unsigned int tasksmackNvmlMockUuidCalls()
    {
        return g_UuidCalls;
    }

    // Test controls (#1116): how many devices NVML reports from the next nvmlDeviceGetCount_v2 on
    // (capped at the mock's two); which device's sensor reads return NVML_ERROR_GPU_IS_LOST
    // (NO_FAILING_HANDLE for none); and how many times nvmlInit_v2 has been called.
    void tasksmackNvmlMockSetDeviceCount(unsigned int count)
    {
        g_DeviceCount = count < MOCK_DEVICES.size() ? count : static_cast<unsigned int>(MOCK_DEVICES.size());
    }

    void tasksmackNvmlMockSetLostDevice(unsigned int index)
    {
        g_LostDeviceIndex = index;
    }

    unsigned int tasksmackNvmlMockInitCalls()
    {
        return g_InitCalls;
    }

    // Test control (#1116): the next `count` nvmlInit_v2 calls fail (see g_FailingInits).
    void tasksmackNvmlMockFailInits(unsigned int count)
    {
        g_FailingInits = count;
    }

    // Test control: how many calls have addressed a device so far (see g_DeviceQueries).
    unsigned int tasksmackNvmlMockDeviceQueries()
    {
        return g_DeviceQueries;
    }

    // Test control (#1270): how many calls have addressed mock device `index`, handle lookups included
    // (see g_QueriesPerDevice); 0 for an index the mock doesn't have.
    unsigned int tasksmackNvmlMockQueriesForDevice(unsigned int index)
    {
        return index < g_QueriesPerDevice.size() ? g_QueriesPerDevice.at(index) : 0U;
    }

    // Test control (#1270 review): while `enabled`, device 0 presents as the GPU that held its slot
    // before (see g_PreviousOccupant).
    void tasksmackNvmlMockSetPreviousOccupant(int enabled)
    {
        g_PreviousOccupant = (enabled != 0);
    }

    // Test control (#1353): mock device `index`'s PCI-info query fails (see g_FailingPciInfoIndex).
    void tasksmackNvmlMockSetFailingPciInfoDevice(unsigned int index)
    {
        g_FailingPciInfoIndex = index;
    }

    const char* nvmlErrorString(NVML::nvmlReturn_t /*result*/)
    {
        return "Mock NVML error";
    }

} // extern "C"
