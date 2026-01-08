#include "NVMLGPUProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <array>
#include <format>

namespace Platform
{

namespace
{

// NVML return codes
constexpr int NVML_SUCCESS = 0;
constexpr int NVML_ERROR_UNINITIALIZED = 1;
constexpr int NVML_ERROR_INVALID_ARGUMENT = 2;
constexpr int NVML_ERROR_NOT_SUPPORTED = 3;
constexpr int NVML_ERROR_NO_PERMISSION = 4;
constexpr int NVML_ERROR_ALREADY_INITIALIZED = 5;
constexpr int NVML_ERROR_NOT_FOUND = 6;
constexpr int NVML_ERROR_INSUFFICIENT_SIZE = 7;
constexpr int NVML_ERROR_INSUFFICIENT_POWER = 8;
constexpr int NVML_ERROR_DRIVER_NOT_LOADED = 9;
constexpr int NVML_ERROR_TIMEOUT = 10;
constexpr int NVML_ERROR_IRQ_ISSUE = 11;
constexpr int NVML_ERROR_LIBRARY_NOT_FOUND = 12;
constexpr int NVML_ERROR_FUNCTION_NOT_FOUND = 13;
constexpr int NVML_ERROR_CORRUPTED_INFOROM = 14;
constexpr int NVML_ERROR_GPU_IS_LOST = 15;

// NVML temperature sensors
constexpr int NVML_TEMPERATURE_GPU = 0;

// NVML clock types
constexpr int NVML_CLOCK_GRAPHICS = 0;
[[maybe_unused]] constexpr int NVML_CLOCK_SM = 1;
constexpr int NVML_CLOCK_MEM = 2;

// NVML PCIe counter types
[[maybe_unused]] constexpr int NVML_PCIE_UTIL_TX_BYTES = 0;
[[maybe_unused]] constexpr int NVML_PCIE_UTIL_RX_BYTES = 1;

// NVML constants
constexpr unsigned int NVML_DEVICE_NAME_BUFFER_SIZE = 64;
constexpr unsigned int NVML_DEVICE_UUID_BUFFER_SIZE = 80;
constexpr unsigned int NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE = 80;
constexpr unsigned int NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE = 32;

// NVML memory info structure
struct nvmlMemory_t
{
    uint64_t total;
    uint64_t free;
    uint64_t used;
};

// NVML utilization structure
struct nvmlUtilization_t
{
    unsigned int gpu;
    unsigned int memory;
};

} // namespace

NVMLGPUProbe::NVMLGPUProbe() : m_Initialized(loadNVML() && initializeNVML())
{
    if (!m_Initialized)
    {
        spdlog::info("NVMLGPUProbe: NVML not available (NVIDIA GPU or driver not detected)");
    }
}

NVMLGPUProbe::~NVMLGPUProbe()
{
    shutdownNVML();
    unloadNVML();
}

bool NVMLGPUProbe::loadNVML()
{
    // Try to load NVML library (nvml.dll on Windows)
    m_NVMLHandle = LoadLibraryA("nvml.dll");
    if (m_NVMLHandle == nullptr)
    {
        spdlog::debug("NVMLGPUProbe: Failed to load nvml.dll (NVIDIA driver not installed)");
        return false;
    }

    // Load function pointers
#define LOAD_NVML_FUNC(name)                                                                                                               \
    m_NVML.name = reinterpret_cast<decltype(m_NVML.name)>(GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvml" #name));               \
    if (m_NVML.name == nullptr)                                                                                                            \
    {                                                                                                                                      \
        spdlog::warn("NVMLGPUProbe: Failed to load nvml" #name);                                                                           \
        return false;                                                                                                                      \
    }

    LOAD_NVML_FUNC(Init)
    LOAD_NVML_FUNC(Shutdown)
    LOAD_NVML_FUNC(DeviceGetCount)
    LOAD_NVML_FUNC(DeviceGetHandleByIndex)
    LOAD_NVML_FUNC(DeviceGetName)
    LOAD_NVML_FUNC(DeviceGetUUID)
    LOAD_NVML_FUNC(DeviceGetMemoryInfo)
    LOAD_NVML_FUNC(DeviceGetTemperature)
    LOAD_NVML_FUNC(DeviceGetPowerUsage)
    LOAD_NVML_FUNC(DeviceGetPowerManagementLimit)
    LOAD_NVML_FUNC(DeviceGetClockInfo)
    LOAD_NVML_FUNC(DeviceGetMaxClockInfo)
    LOAD_NVML_FUNC(DeviceGetUtilizationRates)
    LOAD_NVML_FUNC(DeviceGetPcieThroughput)
    LOAD_NVML_FUNC(DeviceGetDriverVersion)
    LOAD_NVML_FUNC(DeviceGetVbiosVersion)
    LOAD_NVML_FUNC(DeviceGetFanSpeed)

#undef LOAD_NVML_FUNC

    spdlog::debug("NVMLGPUProbe: Successfully loaded nvml.dll");
    return true;
}

void NVMLGPUProbe::unloadNVML()
{
    if (m_NVMLHandle != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(m_NVMLHandle));
        m_NVMLHandle = nullptr;
    }
}

// NOLINTNEXTLINE(readability-make-member-function-const) - Calls NVML init which has global side effects
bool NVMLGPUProbe::initializeNVML()
{
    if (m_NVML.Init == nullptr)
    {
        return false;
    }

    nvmlReturn_t result = m_NVML.Init();
    if (result != NVML_SUCCESS)
    {
        spdlog::warn("NVMLGPUProbe: nvmlInit failed: {}", getNVMLErrorString(result));
        return false;
    }

    // Get driver version
    std::array<char, NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE> driverVersion{};
    result = m_NVML.DeviceGetDriverVersion(driverVersion.data(), NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE);
    if (result == NVML_SUCCESS)
    {
        spdlog::info("NVMLGPUProbe: Initialized with driver version: {}", driverVersion.data());
    }
    else
    {
        spdlog::debug("NVMLGPUProbe: Initialized (driver version unavailable)");
    }

    return true;
}

void NVMLGPUProbe::shutdownNVML()
{
    m_DeviceHandles.clear();

    if (m_Initialized && m_NVML.Shutdown != nullptr)
    {
        m_NVML.Shutdown();
    }

    m_Initialized = false;
}

std::string NVMLGPUProbe::getNVMLErrorString(nvmlReturn_t result)
{
    switch (result)
    {
    case NVML_SUCCESS:
        return "Success";
    case NVML_ERROR_UNINITIALIZED:
        return "Uninitialized";
    case NVML_ERROR_INVALID_ARGUMENT:
        return "Invalid argument";
    case NVML_ERROR_NOT_SUPPORTED:
        return "Not supported";
    case NVML_ERROR_NO_PERMISSION:
        return "No permission";
    case NVML_ERROR_ALREADY_INITIALIZED:
        return "Already initialized";
    case NVML_ERROR_NOT_FOUND:
        return "Not found";
    case NVML_ERROR_INSUFFICIENT_SIZE:
        return "Insufficient size";
    case NVML_ERROR_INSUFFICIENT_POWER:
        return "Insufficient power";
    case NVML_ERROR_DRIVER_NOT_LOADED:
        return "Driver not loaded";
    case NVML_ERROR_TIMEOUT:
        return "Timeout";
    case NVML_ERROR_IRQ_ISSUE:
        return "IRQ issue";
    case NVML_ERROR_LIBRARY_NOT_FOUND:
        return "Library not found";
    case NVML_ERROR_FUNCTION_NOT_FOUND:
        return "Function not found";
    case NVML_ERROR_CORRUPTED_INFOROM:
        return "Corrupted InfoROM";
    case NVML_ERROR_GPU_IS_LOST:
        return "GPU is lost";
    default:
        return std::format("Unknown error ({})", result);
    }
}

std::vector<GPUInfo> NVMLGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    if (!m_Initialized)
    {
        return gpus;
    }

    // Get device count
    unsigned int deviceCount = 0;
    nvmlReturn_t result = m_NVML.DeviceGetCount(&deviceCount);
    if (result != NVML_SUCCESS)
    {
        spdlog::warn("NVMLGPUProbe: DeviceGetCount failed: {}", getNVMLErrorString(result));
        return gpus;
    }

    // Enumerate devices
    for (unsigned int i = 0; i < deviceCount; ++i)
    {
        nvmlDevice_t device = nullptr;
        result = m_NVML.DeviceGetHandleByIndex(i, &device);
        if (result != NVML_SUCCESS || device == nullptr)
        {
            spdlog::warn("NVMLGPUProbe: DeviceGetHandleByIndex({}) failed: {}", i, getNVMLErrorString(result));
            continue;
        }

        // Store device handle for later use
        m_DeviceHandles[i] = device;

        GPUInfo info{};

        // Get device name
        std::array<char, NVML_DEVICE_NAME_BUFFER_SIZE> name{};
        result = m_NVML.DeviceGetName(device, name.data(), NVML_DEVICE_NAME_BUFFER_SIZE);
        if (result == NVML_SUCCESS)
        {
            info.name = name.data();
        }

        // Get device UUID (unique identifier)
        std::array<char, NVML_DEVICE_UUID_BUFFER_SIZE> uuid{};
        result = m_NVML.DeviceGetUUID(device, uuid.data(), NVML_DEVICE_UUID_BUFFER_SIZE);
        if (result == NVML_SUCCESS)
        {
            info.id = uuid.data();
        }
        else
        {
            // Fallback to index-based ID
            info.id = std::format("NVML_GPU{}", i);
        }

        // NVML only works with NVIDIA GPUs
        info.vendor = "NVIDIA";

        // Get VBIOS version (driver version)
        std::array<char, NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE> vbiosVersion{};
        result = m_NVML.DeviceGetVbiosVersion(device, vbiosVersion.data(), NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE);
        if (result == NVML_SUCCESS)
        {
            info.driverVersion = vbiosVersion.data();
        }

        // NVIDIA discrete GPUs (NVML doesn't expose integrated GPUs typically)
        info.isIntegrated = false;

        info.deviceIndex = i;

        spdlog::debug("NVMLGPUProbe: Enumerated NVIDIA GPU {}: {}", i, info.name);

        gpus.push_back(std::move(info));
    }

    spdlog::info("NVMLGPUProbe: Enumerated {} NVIDIA GPU(s)", gpus.size());
    return gpus;
}

std::vector<GPUCounters> NVMLGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    if (!m_Initialized)
    {
        return counters;
    }

    for (const auto& [index, device] : m_DeviceHandles)
    {
        GPUCounters counter{};

        // Get UUID for ID
        std::array<char, NVML_DEVICE_UUID_BUFFER_SIZE> uuid{};
        nvmlReturn_t result = m_NVML.DeviceGetUUID(device, uuid.data(), NVML_DEVICE_UUID_BUFFER_SIZE);
        if (result == NVML_SUCCESS)
        {
            counter.gpuId = uuid.data();
        }
        else
        {
            counter.gpuId = std::format("NVML_GPU{}", index);
        }

        // Memory info (raw counters only)
        nvmlMemory_t memInfo{};
        result = m_NVML.DeviceGetMemoryInfo(device, &memInfo);
        if (result == NVML_SUCCESS)
        {
            counter.memoryUsedBytes = memInfo.used;
            counter.memoryTotalBytes = memInfo.total;
        }

        // Temperature (GPU die)
        unsigned int temp = 0;
        result = m_NVML.DeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp);
        if (result == NVML_SUCCESS)
        {
            counter.temperatureC = static_cast<int32_t>(temp);
        }

        // Power usage (milliwatts) - raw counter only
        unsigned int powerMilliwatts = 0;
        result = m_NVML.DeviceGetPowerUsage(device, &powerMilliwatts);
        if (result == NVML_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMilliwatts) / 1000.0;
        }

        // Power limit (milliwatts) - raw counter only
        unsigned int powerLimitMilliwatts = 0;
        result = m_NVML.DeviceGetPowerManagementLimit(device, &powerLimitMilliwatts);
        if (result == NVML_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerLimitMilliwatts) / 1000.0;
        }

        // GPU clock (MHz)
        unsigned int gpuClock = 0;
        result = m_NVML.DeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &gpuClock);
        if (result == NVML_SUCCESS)
        {
            counter.gpuClockMHz = gpuClock;
        }

        // Memory clock (MHz)
        unsigned int memClock = 0;
        result = m_NVML.DeviceGetClockInfo(device, NVML_CLOCK_MEM, &memClock);
        if (result == NVML_SUCCESS)
        {
            counter.memoryClockMHz = memClock;
        }

        // GPU utilization
        nvmlUtilization_t util{};
        result = m_NVML.DeviceGetUtilizationRates(device, &util);
        if (result == NVML_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(util.gpu);
        }

        // Fan speed (NVML returns percentage 0-100)
        unsigned int fanSpeed = 0;
        result = m_NVML.DeviceGetFanSpeed(device, &fanSpeed);
        if (result == NVML_SUCCESS)
        {
            counter.fanSpeedRPMPercent = fanSpeed;
        }

        // PCIe throughput: NVML returns rates (KB/s), not cumulative counters.
        // GPUTypes.h expects cumulative pcieTxBytes/pcieRxBytes.
        // Since NVML doesn't provide cumulative counters, we leave these at 0.
        // Future enhancement: Add rate fields or implement tracking.
        // For now, Domain layer will compute rates as 0 from cumulative fields.

        counters.push_back(std::move(counter));
    }

    return counters;
}

std::vector<ProcessGPUCounters> NVMLGPUProbe::readProcessGPUCounters()
{
    // Per-process GPU metrics via NVML will be implemented in Phase 3
    // Windows: Use D3DKMT APIs for per-process metrics (all vendors)
    // NVML can provide additional NVIDIA-specific per-process data
    return {};
}

GPUCapabilities NVMLGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!m_Initialized)
    {
        return caps;
    }

    // NVML provides comprehensive capabilities for NVIDIA GPUs
    caps.hasTemperature = true;
    caps.hasHotspotTemp = false; // Not exposed via standard NVML APIs
    caps.hasPowerMetrics = true;
    caps.hasClockSpeeds = true;
    caps.hasFanSpeed = true;
    caps.hasPCIeMetrics = true;
    caps.hasEngineUtilization = true;
    caps.hasPerProcessMetrics = false; // Will be implemented via D3DKMT in Phase 3
    caps.hasEncoderDecoder = false;    // Not implemented yet
    caps.supportsMultiGPU = true;

    return caps;
}

} // namespace Platform
