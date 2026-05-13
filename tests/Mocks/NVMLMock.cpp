#include "Platform/NVMLTypes.h"

#include <algorithm>
#include <array>
#include <cstring>

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
     .memory = {.total = 8ULL * 1024ULL * 1024ULL * 1024ULL, .free = 6ULL * 1024ULL * 1024ULL * 1024ULL, .used = 2ULL * 1024ULL * 1024ULL * 1024ULL},
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
     .memory = {.total = 16ULL * 1024ULL * 1024ULL * 1024ULL, .free = 10ULL * 1024ULL * 1024ULL * 1024ULL, .used = 6ULL * 1024ULL * 1024ULL * 1024ULL},
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

std::array<int, MOCK_DEVICES.size()> MOCK_HANDLES{1, 2};

[[nodiscard]] auto deviceIndex(NVML::nvmlDevice_t device) -> std::size_t
{
    const auto match = std::ranges::find(MOCK_HANDLES, device);
    return static_cast<std::size_t>(std::distance(MOCK_HANDLES.begin(), match));
}

void writeString(const char* source, char* destination, unsigned int length)
{
    std::strncpy(destination, source, length);
    destination[length - 1] = '\0';
}

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
    *device = &MOCK_HANDLES.at(index);
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetName(NVML::nvmlDevice_t device, char* name, unsigned int length)
{
    writeString(MOCK_DEVICES.at(deviceIndex(device)).name, name, length);
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetUUID(NVML::nvmlDevice_t device, char* uuid, unsigned int length)
{
    const auto& mockDevice = MOCK_DEVICES.at(deviceIndex(device));
    if (!mockDevice.hasUuid)
    {
        return NVML::NVML_ERROR_NOT_FOUND;
    }

    writeString(mockDevice.uuid, uuid, length);
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetMemoryInfo(NVML::nvmlDevice_t device, NVML::nvmlMemory_t* memory)
{
    *memory = MOCK_DEVICES.at(deviceIndex(device)).memory;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetUtilizationRates(NVML::nvmlDevice_t device, NVML::nvmlUtilization_t* utilization)
{
    utilization->gpu = MOCK_DEVICES.at(deviceIndex(device)).utilizationPercent;
    utilization->memory = 0;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetTemperature(NVML::nvmlDevice_t device, NVML::nvmlTemperatureSensors_t /*sensor*/, unsigned int* temperature)
{
    *temperature = MOCK_DEVICES.at(deviceIndex(device)).temperatureC;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetPowerUsage(NVML::nvmlDevice_t device, unsigned int* power)
{
    *power = MOCK_DEVICES.at(deviceIndex(device)).powerMilliwatts;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetPowerManagementLimit(NVML::nvmlDevice_t device, unsigned int* limit)
{
    *limit = MOCK_DEVICES.at(deviceIndex(device)).powerLimitMilliwatts;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetClockInfo(NVML::nvmlDevice_t device, NVML::nvmlClockType_t type, unsigned int* clock)
{
    const auto& mockDevice = MOCK_DEVICES.at(deviceIndex(device));
    *clock = (type == NVML::NVML_CLOCK_MEM) ? mockDevice.memoryClockMHz : mockDevice.graphicsClockMHz;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetFanSpeed(NVML::nvmlDevice_t device, unsigned int* fanSpeed)
{
    *fanSpeed = MOCK_DEVICES.at(deviceIndex(device)).fanPercent;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetPcieThroughput(NVML::nvmlDevice_t device, NVML::nvmlPcieUtilCounter_t counter, unsigned int* throughput)
{
    const auto& mockDevice = MOCK_DEVICES.at(deviceIndex(device));
    *throughput = (counter == NVML::NVML_PCIE_UTIL_TX_BYTES) ? mockDevice.pcieTxKilobytes : mockDevice.pcieRxKilobytes;
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfo_t* infos)
{
    if (deviceIndex(device) != 0)
    {
        *count = 0;
        return NVML::NVML_SUCCESS;
    }

    *count = 1;
    if (infos == nullptr)
    {
        return NVML::NVML_SUCCESS;
    }

    infos[0] = {.pid = 123U, .usedGpuMemory = 111ULL, .gpuInstanceId = 0U, .computeInstanceId = 0U};
    return NVML::NVML_SUCCESS;
}

NVML::nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(NVML::nvmlDevice_t device, unsigned int* count, NVML::nvmlProcessInfo_t* infos)
{
    if (deviceIndex(device) != 0)
    {
        *count = 0;
        return NVML::NVML_SUCCESS;
    }

    *count = 2;
    if (infos == nullptr)
    {
        return NVML::NVML_SUCCESS;
    }

    infos[0] = {.pid = 123U, .usedGpuMemory = 222ULL, .gpuInstanceId = 0U, .computeInstanceId = 0U};
    infos[1] = {.pid = 456U, .usedGpuMemory = 333ULL, .gpuInstanceId = 0U, .computeInstanceId = 0U};
    return NVML::NVML_SUCCESS;
}

const char* nvmlErrorString(NVML::nvmlReturn_t /*result*/)
{
    return "Mock NVML error";
}

} // extern "C"
