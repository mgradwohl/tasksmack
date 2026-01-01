#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>

// NVML headers (dynamically loaded)
#include <nvml.h>

namespace Platform
{

struct NVMLGPUProbe::Impl
{
    void* nvmlHandle = nullptr;
    bool initialized = false;
    std::uint32_t deviceCount = 0;
    std::vector<nvmlDevice_t> devices;

    // NVML function pointers
    nvmlReturn_t (*nvmlInit_v2)() = nullptr;
    nvmlReturn_t (*nvmlShutdown)() = nullptr;
    nvmlReturn_t (*nvmlDeviceGetCount_v2)(unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetName)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetUUID)(nvmlDevice_t, char*, unsigned int) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetMemoryInfo)(nvmlDevice_t, nvmlMemory_t*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetUtilizationRates)(nvmlDevice_t, nvmlUtilization_t*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetTemperature)(nvmlDevice_t, nvmlTemperatureSensors_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetPowerUsage)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetClockInfo)(nvmlDevice_t, nvmlClockType_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetFanSpeed)(nvmlDevice_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetPcieThroughput)(nvmlDevice_t, nvmlPcieUtilCounter_t, unsigned int*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetComputeRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetGraphicsRunningProcesses)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*) = nullptr;
    const char* (*nvmlErrorString)(nvmlReturn_t) = nullptr;

    bool loadNVML();
    void unloadNVML();
    [[nodiscard]] std::string getNVMLError(nvmlReturn_t result) const;
};

bool NVMLGPUProbe::Impl::loadNVML()
{
    if (initialized)
    {
        return true;
    }

    // Try to load libnvidia-ml.so (dynamic loading for graceful fallback)
    nvmlHandle = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (nvmlHandle == nullptr)
    {
        nvmlHandle = dlopen("libnvidia-ml.so", RTLD_NOW);
        if (nvmlHandle == nullptr)
        {
            spdlog::debug("NVMLGPUProbe: Failed to load libnvidia-ml.so - {}", dlerror());
            return false;
        }
    }

// Load function pointers
#define LOAD_NVML_FUNC(name)                                                                                                               \
    name = reinterpret_cast<decltype(name)>(dlsym(nvmlHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::error("NVMLGPUProbe: Failed to load symbol " #name);                                                                       \
        unloadNVML();                                                                                                                      \
        return false;                                                                                                                      \
    }

    LOAD_NVML_FUNC(nvmlInit_v2);
    LOAD_NVML_FUNC(nvmlShutdown);
    LOAD_NVML_FUNC(nvmlDeviceGetCount_v2);
    LOAD_NVML_FUNC(nvmlDeviceGetHandleByIndex_v2);
    LOAD_NVML_FUNC(nvmlDeviceGetName);
    LOAD_NVML_FUNC(nvmlDeviceGetUUID);
    LOAD_NVML_FUNC(nvmlDeviceGetMemoryInfo);
    LOAD_NVML_FUNC(nvmlDeviceGetUtilizationRates);
    LOAD_NVML_FUNC(nvmlDeviceGetTemperature);
    LOAD_NVML_FUNC(nvmlDeviceGetPowerUsage);
    LOAD_NVML_FUNC(nvmlDeviceGetPowerManagementLimit);
    LOAD_NVML_FUNC(nvmlDeviceGetClockInfo);
    LOAD_NVML_FUNC(nvmlDeviceGetFanSpeed);
    LOAD_NVML_FUNC(nvmlDeviceGetPcieThroughput);
    LOAD_NVML_FUNC(nvmlDeviceGetComputeRunningProcesses);
    LOAD_NVML_FUNC(nvmlDeviceGetGraphicsRunningProcesses);
    LOAD_NVML_FUNC(nvmlErrorString);

#undef LOAD_NVML_FUNC

    // Initialize NVML
    auto result = nvmlInit_v2();
    if (result != NVML_SUCCESS)
    {
        spdlog::error("NVMLGPUProbe: nvmlInit_v2 failed - {}", getNVMLError(result));
        unloadNVML();
        return false;
    }

    // Get device count
    result = nvmlDeviceGetCount_v2(&deviceCount);
    if (result != NVML_SUCCESS)
    {
        spdlog::error("NVMLGPUProbe: nvmlDeviceGetCount_v2 failed - {}", getNVMLError(result));
        nvmlShutdown();
        unloadNVML();
        return false;
    }

    // Get device handles
    devices.resize(deviceCount);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        result = nvmlDeviceGetHandleByIndex_v2(i, &devices[i]);
        if (result != NVML_SUCCESS)
        {
            spdlog::warn("NVMLGPUProbe: Failed to get handle for GPU {} - {}", i, getNVMLError(result));
        }
    }

    initialized = true;
    spdlog::info("NVMLGPUProbe: Initialized successfully, found {} NVIDIA GPU(s)", deviceCount);
    return true;
}

void NVMLGPUProbe::Impl::unloadNVML()
{
    if (initialized && nvmlShutdown != nullptr)
    {
        nvmlShutdown();
    }

    if (nvmlHandle != nullptr)
    {
        dlclose(nvmlHandle);
        nvmlHandle = nullptr;
    }

    initialized = false;
    deviceCount = 0;
    devices.clear();
}

std::string NVMLGPUProbe::Impl::getNVMLError(nvmlReturn_t result) const
{
    if (nvmlErrorString != nullptr)
    {
        const char* errorStr = nvmlErrorString(result);
        if (errorStr != nullptr)
        {
            return {errorStr};
        }
    }
    return "Unknown NVML error";
}

// Constructor
NVMLGPUProbe::NVMLGPUProbe() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->loadNVML();
}

// Destructor
NVMLGPUProbe::~NVMLGPUProbe()
{
    if (m_Impl)
    {
        m_Impl->unloadNVML();
    }
}

bool NVMLGPUProbe::isAvailable() const
{
    return m_Impl && m_Impl->initialized;
}

std::vector<GPUInfo> NVMLGPUProbe::enumerateGPUs()
{
    if (!isAvailable())
    {
        return {};
    }

    std::vector<GPUInfo> gpus;
    gpus.reserve(m_Impl->deviceCount);

    for (std::uint32_t i = 0; i < m_Impl->deviceCount; ++i)
    {
        nvmlDevice_t device = m_Impl->devices[i];

        GPUInfo info;
        info.deviceIndex = i;
        info.vendor = "NVIDIA";
        info.isIntegrated = false; // NVIDIA GPUs are typically discrete

        // Get GPU name
        char name[NVML_DEVICE_NAME_BUFFER_SIZE]{};
        auto result = m_Impl->nvmlDeviceGetName(device, name, sizeof(name));
        if (result == NVML_SUCCESS)
        {
            info.name = name;
        }

        // Get UUID as ID
        char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
        result = m_Impl->nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
        if (result == NVML_SUCCESS)
        {
            info.id = uuid;
        }
        else
        {
            // Fallback to index-based ID
            info.id = "nvidia-" + std::to_string(i);
        }

        gpus.push_back(std::move(info));
    }

    return gpus;
}

std::vector<GPUCounters> NVMLGPUProbe::readGPUCounters()
{
    if (!isAvailable())
    {
        return {};
    }

    std::vector<GPUCounters> counters;
    counters.reserve(m_Impl->deviceCount);

    for (std::uint32_t i = 0; i < m_Impl->deviceCount; ++i)
    {
        nvmlDevice_t device = m_Impl->devices[i];
        GPUCounters counter;

        // Get UUID as GPU ID
        char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
        auto result = m_Impl->nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
        if (result == NVML_SUCCESS)
        {
            counter.gpuId = uuid;
        }
        else
        {
            counter.gpuId = "nvidia-" + std::to_string(i);
        }

        // Memory info
        nvmlMemory_t memInfo{};
        result = m_Impl->nvmlDeviceGetMemoryInfo(device, &memInfo);
        if (result == NVML_SUCCESS)
        {
            counter.memoryUsedBytes = memInfo.used;
            counter.memoryTotalBytes = memInfo.total;
            // Note: memoryUtilPercent is computed in Domain layer from raw bytes
        }

        // Utilization
        nvmlUtilization_t util{};
        result = m_Impl->nvmlDeviceGetUtilizationRates(device, &util);
        if (result == NVML_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(util.gpu);
        }

        // Temperature
        unsigned int temp = 0;
        result = m_Impl->nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp);
        if (result == NVML_SUCCESS)
        {
            counter.temperatureC = static_cast<std::int32_t>(temp);
        }

        // Power
        unsigned int powerMilliwatts = 0;
        result = m_Impl->nvmlDeviceGetPowerUsage(device, &powerMilliwatts);
        if (result == NVML_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMilliwatts) / 1000.0;
        }

        unsigned int powerLimitMilliwatts = 0;
        result = m_Impl->nvmlDeviceGetPowerManagementLimit(device, &powerLimitMilliwatts);
        if (result == NVML_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerLimitMilliwatts) / 1000.0;
        }

        // Clock speeds
        unsigned int gpuClock = 0;
        result = m_Impl->nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &gpuClock);
        if (result == NVML_SUCCESS)
        {
            counter.gpuClockMHz = gpuClock;
        }

        unsigned int memClock = 0;
        result = m_Impl->nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &memClock);
        if (result == NVML_SUCCESS)
        {
            counter.memoryClockMHz = memClock;
        }

        // Fan speed
        unsigned int fanSpeed = 0;
        result = m_Impl->nvmlDeviceGetFanSpeed(device, &fanSpeed);
        if (result == NVML_SUCCESS)
        {
            counter.fanSpeedRPM = fanSpeed;
        }

        // PCIe throughput
        unsigned int pcieTx = 0;
        result = m_Impl->nvmlDeviceGetPcieThroughput(device, NVML_PCIE_UTIL_TX_BYTES, &pcieTx);
        if (result == NVML_SUCCESS)
        {
            counter.pcieTxBytes = static_cast<std::uint64_t>(pcieTx) * 1024; // KB to bytes
        }

        unsigned int pcieRx = 0;
        result = m_Impl->nvmlDeviceGetPcieThroughput(device, NVML_PCIE_UTIL_RX_BYTES, &pcieRx);
        if (result == NVML_SUCCESS)
        {
            counter.pcieRxBytes = static_cast<std::uint64_t>(pcieRx) * 1024; // KB to bytes
        }

        counters.push_back(std::move(counter));
    }

    return counters;
}

std::vector<ProcessGPUCounters> NVMLGPUProbe::readProcessGPUCounters()
{
    if (!isAvailable())
    {
        return {};
    }

    std::vector<ProcessGPUCounters> allCounters;

    for (std::uint32_t i = 0; i < m_Impl->deviceCount; ++i)
    {
        nvmlDevice_t device = m_Impl->devices[i];

        // Get UUID as GPU ID
        char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
        std::string gpuId;
        auto result = m_Impl->nvmlDeviceGetUUID(device, uuid, sizeof(uuid));
        if (result == NVML_SUCCESS)
        {
            gpuId = uuid;
        }
        else
        {
            gpuId = "nvidia-" + std::to_string(i);
        }

        // Query compute processes
        unsigned int computeCount = 0;
        result = m_Impl->nvmlDeviceGetComputeRunningProcesses(device, &computeCount, nullptr);
        if (result == NVML_SUCCESS && computeCount > 0)
        {
            std::vector<nvmlProcessInfo_t> computeProcesses(computeCount);
            result = m_Impl->nvmlDeviceGetComputeRunningProcesses(device, &computeCount, computeProcesses.data());
            if (result == NVML_SUCCESS)
            {
                for (const auto& proc : computeProcesses)
                {
                    ProcessGPUCounters counter;
                    counter.pid = static_cast<std::int32_t>(proc.pid);
                    counter.gpuId = gpuId;
                    counter.gpuMemoryBytes = proc.usedGpuMemory;
                    counter.activeEngines.push_back("Compute");
                    allCounters.push_back(std::move(counter));
                }
            }
        }

        // Query graphics processes
        unsigned int graphicsCount = 0;
        result = m_Impl->nvmlDeviceGetGraphicsRunningProcesses(device, &graphicsCount, nullptr);
        if (result == NVML_SUCCESS && graphicsCount > 0)
        {
            std::vector<nvmlProcessInfo_t> graphicsProcesses(graphicsCount);
            result = m_Impl->nvmlDeviceGetGraphicsRunningProcesses(device, &graphicsCount, graphicsProcesses.data());
            if (result == NVML_SUCCESS)
            {
                for (const auto& proc : graphicsProcesses)
                {
                    // Check if we already have this process from compute list
                    auto it = std::ranges::find_if(allCounters,
                                                   [&proc, &gpuId](const ProcessGPUCounters& c)
                                                   { return c.pid == static_cast<std::int32_t>(proc.pid) && c.gpuId == gpuId; });

                    if (it != allCounters.end())
                    {
                        // Merge: add graphics engine
                        it->activeEngines.push_back("3D");
                        it->gpuMemoryBytes = std::max(it->gpuMemoryBytes, proc.usedGpuMemory);
                    }
                    else
                    {
                        ProcessGPUCounters counter;
                        counter.pid = static_cast<std::int32_t>(proc.pid);
                        counter.gpuId = gpuId;
                        counter.gpuMemoryBytes = proc.usedGpuMemory;
                        counter.activeEngines.push_back("3D");
                        allCounters.push_back(std::move(counter));
                    }
                }
            }
        }
    }

    return allCounters;
}

GPUCapabilities NVMLGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (isAvailable())
    {
        caps.hasTemperature = true;
        caps.hasPowerMetrics = true;
        caps.hasClockSpeeds = true;
        caps.hasFanSpeed = true;
        caps.hasPCIeMetrics = true;
        caps.hasPerProcessMetrics = true;
        caps.supportsMultiGPU = true;
        caps.hasEngineUtilization = true; // Via activeEngines in ProcessGPUCounters
    }

    return caps;
}

} // namespace Platform
