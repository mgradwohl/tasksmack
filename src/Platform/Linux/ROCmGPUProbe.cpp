#include "ROCmGPUProbe.h"

#include "Platform/GPUTypes.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <dlfcn.h>

// ROCm SMI types and constants (for dynamic loading without requiring rocm_smi.h)
// These definitions match the ROCm SMI API but don't require ROCm installation
namespace
{

// NOLINTBEGIN(readability-identifier-naming) - these types mirror AMD ROCm SMI C API naming
using rsmi_device_t = std::uint32_t;
using rsmi_status_t = std::uint32_t;
// NOLINTEND(readability-identifier-naming)

// ROCm SMI return codes
constexpr rsmi_status_t RSMI_STATUS_SUCCESS = 0;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INVALID_ARGS = 1;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_NOT_SUPPORTED = 2;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_FILE_ERROR = 3;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_PERMISSION = 4;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_OUT_OF_RESOURCES = 5;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INTERNAL_EXCEPTION = 6;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INPUT_OUT_OF_BOUNDS = 7;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INIT_ERROR = 8;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_NOT_YET_IMPLEMENTED = 9;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_NOT_FOUND = 10;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INSUFFICIENT_SIZE = 11;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_INTERRUPT = 12;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_UNEXPECTED_SIZE = 13;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_NO_DATA = 14;
[[maybe_unused]] constexpr rsmi_status_t RSMI_STATUS_UNKNOWN_ERROR = 0xFFFFFFFF;

// ROCm SMI temperature types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_temperature_type_t : std::uint32_t
{
    RSMI_TEMP_TYPE_EDGE = 0,
    RSMI_TEMP_TYPE_JUNCTION = 1,
    RSMI_TEMP_TYPE_MEMORY = 2,
    RSMI_TEMP_TYPE_HBM_0 = 3,
    RSMI_TEMP_TYPE_HBM_1 = 4,
    RSMI_TEMP_TYPE_HBM_2 = 5,
    RSMI_TEMP_TYPE_HBM_3 = 6
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI temperature metric
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_temperature_metric_t : std::uint32_t
{
    RSMI_TEMP_CURRENT = 0,
    RSMI_TEMP_MAX = 1,
    RSMI_TEMP_MIN = 2,
    RSMI_TEMP_MAX_HYST = 3,
    RSMI_TEMP_MIN_HYST = 4,
    RSMI_TEMP_CRITICAL = 5,
    RSMI_TEMP_CRITICAL_HYST = 6,
    RSMI_TEMP_EMERGENCY = 7,
    RSMI_TEMP_EMERGENCY_HYST = 8,
    RSMI_TEMP_CRIT_MIN = 9,
    RSMI_TEMP_CRIT_MIN_HYST = 10,
    RSMI_TEMP_OFFSET = 11,
    RSMI_TEMP_LOWEST = 12,
    RSMI_TEMP_HIGHEST = 13
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI clock types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_clk_type_t : std::uint32_t
{
    RSMI_CLK_TYPE_SYS = 0,
    RSMI_CLK_TYPE_DF = 1,
    RSMI_CLK_TYPE_DCEF = 2,
    RSMI_CLK_TYPE_SOC = 3,
    RSMI_CLK_TYPE_MEM = 4,
    RSMI_CLK_TYPE_FIRST = RSMI_CLK_TYPE_SYS,
    RSMI_CLK_TYPE_LAST = RSMI_CLK_TYPE_MEM
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI memory types
// NOLINTBEGIN(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming) - must match AMD ROCm SMI API
enum rsmi_memory_type_t : std::uint32_t
{
    RSMI_MEM_TYPE_VRAM = 0,
    RSMI_MEM_TYPE_VIS_VRAM = 1,
    RSMI_MEM_TYPE_GTT = 2,
    RSMI_MEM_TYPE_FIRST = RSMI_MEM_TYPE_VRAM,
    RSMI_MEM_TYPE_LAST = RSMI_MEM_TYPE_GTT
};
// NOLINTEND(cppcoreguidelines-use-enum-class,performance-enum-size,readability-identifier-naming)

// ROCm SMI buffer size constants
constexpr std::size_t RSMI_MAX_BUFFER_LENGTH = 256;

// ROCm SMI frequency structure
// NOLINTNEXTLINE(readability-identifier-naming) - must match AMD ROCm SMI API
struct rsmi_frequencies_t
{
    std::uint32_t num_supported;
    std::uint32_t current;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - must match AMD ROCm SMI API ABI
    std::uint64_t frequency[32];
};

} // anonymous namespace

namespace Platform
{

struct ROCmGPUProbe::Impl
{
    void* rocmHandle = nullptr;
    bool initialized = false;
    std::uint32_t deviceCount = 0;
    std::vector<rsmi_device_t> devices;

    // ROCm SMI function pointers
    rsmi_status_t (*rsmi_init)(std::uint64_t) = nullptr;
    rsmi_status_t (*rsmi_shut_down)() = nullptr;
    rsmi_status_t (*rsmi_num_monitor_devices)(std::uint32_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_name_get)(std::uint32_t, char*, std::size_t) = nullptr;
    rsmi_status_t (*rsmi_dev_id_get)(std::uint32_t, std::uint16_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_pci_id_get)(std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_unique_id_get)(std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_gpu_busy_percent_get)(std::uint32_t, std::uint32_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_memory_usage_get)(std::uint32_t, rsmi_memory_type_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_memory_total_get)(std::uint32_t, rsmi_memory_type_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_temp_metric_get)(std::uint32_t, rsmi_temperature_type_t, rsmi_temperature_metric_t, std::int64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_power_ave_get)(std::uint32_t, std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_power_cap_get)(std::uint32_t, std::uint32_t, std::uint64_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_gpu_clk_freq_get)(std::uint32_t, rsmi_clk_type_t, rsmi_frequencies_t*) = nullptr;
    rsmi_status_t (*rsmi_dev_fan_speed_get)(std::uint32_t, std::uint32_t, std::int64_t*) = nullptr;
    const char* (*rsmi_status_string)(rsmi_status_t) = nullptr;

    bool loadROCmSMI();
    void unloadROCmSMI();
    [[nodiscard]] std::string getROCmError(rsmi_status_t result) const;
};

bool ROCmGPUProbe::Impl::loadROCmSMI()
{
    if (initialized)
    {
        return true;
    }

    // Try to load librocm_smi64.so (dynamic loading for graceful fallback)
    rocmHandle = dlopen("librocm_smi64.so.6", RTLD_NOW);
    if (rocmHandle == nullptr)
    {
        rocmHandle = dlopen("librocm_smi64.so", RTLD_NOW);
        if (rocmHandle == nullptr)
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe) - dlerror() is safe in single-threaded init
            spdlog::debug("ROCmGPUProbe: Failed to load librocm_smi64.so - {}", dlerror());
            return false;
        }
    }

// Load function pointers
// Note: dlsym returns void* by POSIX definition; reinterpret_cast to the function pointer type is required
// and safe here because we only use it for known ROCm SMI symbols with matching signatures.
// NOLINTBEGIN(concurrency-mt-unsafe,bugprone-macro-parentheses) - dlerror() safe in single-threaded init, macro pattern is correct
#define LOAD_ROCM_FUNC(name)                                                                                                               \
    name = reinterpret_cast<decltype(name)>(dlsym(rocmHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::warn("ROCmGPUProbe: Failed to load function {} - {}", #name, dlerror());                                                   \
        unloadROCmSMI();                                                                                                                   \
        return false;                                                                                                                      \
    }

    LOAD_ROCM_FUNC(rsmi_init);
    LOAD_ROCM_FUNC(rsmi_shut_down);
    LOAD_ROCM_FUNC(rsmi_num_monitor_devices);
    LOAD_ROCM_FUNC(rsmi_dev_name_get);
    LOAD_ROCM_FUNC(rsmi_dev_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_pci_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_unique_id_get);
    LOAD_ROCM_FUNC(rsmi_dev_gpu_busy_percent_get);
    LOAD_ROCM_FUNC(rsmi_dev_memory_usage_get);
    LOAD_ROCM_FUNC(rsmi_dev_memory_total_get);
    LOAD_ROCM_FUNC(rsmi_dev_temp_metric_get);
    LOAD_ROCM_FUNC(rsmi_dev_power_ave_get);
    LOAD_ROCM_FUNC(rsmi_dev_power_cap_get);
    LOAD_ROCM_FUNC(rsmi_dev_gpu_clk_freq_get);
    LOAD_ROCM_FUNC(rsmi_dev_fan_speed_get);
    LOAD_ROCM_FUNC(rsmi_status_string);

#undef LOAD_ROCM_FUNC
    // NOLINTEND(concurrency-mt-unsafe,bugprone-macro-parentheses)

    // Initialize ROCm SMI (flags = 0 for default initialization)
    rsmi_status_t result = rsmi_init(0);
    if (result != RSMI_STATUS_SUCCESS)
    {
        spdlog::warn("ROCmGPUProbe: Failed to initialize ROCm SMI - {}", getROCmError(result));
        unloadROCmSMI();
        return false;
    }

    // Get device count
    result = rsmi_num_monitor_devices(&deviceCount);
    if (result != RSMI_STATUS_SUCCESS || deviceCount == 0)
    {
        spdlog::debug("ROCmGPUProbe: No AMD GPUs found or failed to get device count - {}", getROCmError(result));
        rsmi_shut_down();
        unloadROCmSMI();
        return false;
    }

    // Populate device handles
    devices.clear();
    devices.reserve(deviceCount);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        devices.push_back(i);
    }

    initialized = true;
    spdlog::info("ROCmGPUProbe: Initialized successfully with {} AMD GPU(s)", deviceCount);
    return true;
}

void ROCmGPUProbe::Impl::unloadROCmSMI()
{
    if (rocmHandle != nullptr)
    {
        dlclose(rocmHandle);
        rocmHandle = nullptr;
    }
    initialized = false;
    deviceCount = 0;
    devices.clear();
}

std::string ROCmGPUProbe::Impl::getROCmError(rsmi_status_t result) const
{
    if (rsmi_status_string != nullptr)
    {
        const char* errStr = rsmi_status_string(result);
        if (errStr != nullptr)
        {
            return errStr;
        }
    }
    return "Unknown ROCm error " + std::to_string(result);
}

ROCmGPUProbe::ROCmGPUProbe() : m_Impl(std::make_unique<Impl>())
{
    m_Impl->loadROCmSMI();
}

ROCmGPUProbe::~ROCmGPUProbe()
{
    if (m_Impl && m_Impl->initialized && m_Impl->rsmi_shut_down != nullptr)
    {
        m_Impl->rsmi_shut_down();
    }
    m_Impl->unloadROCmSMI();
}

bool ROCmGPUProbe::isAvailable() const
{
    return m_Impl && m_Impl->initialized;
}

std::vector<GPUInfo> ROCmGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    if (!isAvailable())
    {
        return gpus;
    }

    gpus.reserve(m_Impl->deviceCount);

    for (std::uint32_t deviceIdx = 0; deviceIdx < m_Impl->deviceCount; ++deviceIdx)
    {
        GPUInfo info{};
        info.deviceIndex = deviceIdx;
        info.vendor = "AMD";
        info.isIntegrated = false; // ROCm typically monitors discrete AMD GPUs

        // Get device name
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - C API buffer
        char nameBuf[RSMI_MAX_BUFFER_LENGTH] = {};
        rsmi_status_t result = m_Impl->rsmi_dev_name_get(deviceIdx, nameBuf, sizeof(nameBuf));
        if (result == RSMI_STATUS_SUCCESS)
        {
            info.name = nameBuf;
        }
        else
        {
            info.name = "AMD GPU " + std::to_string(deviceIdx);
        }

        // Get unique ID (use PCI ID as fallback if unique ID unavailable)
        std::uint64_t uniqueId = 0;
        result = m_Impl->rsmi_dev_unique_id_get(deviceIdx, &uniqueId);
        if (result == RSMI_STATUS_SUCCESS)
        {
            info.id = std::to_string(uniqueId);
        }
        else
        {
            // Fallback: use PCI ID
            std::uint64_t pciId = 0;
            result = m_Impl->rsmi_dev_pci_id_get(deviceIdx, &pciId);
            if (result == RSMI_STATUS_SUCCESS)
            {
                info.id = std::to_string(pciId);
            }
            else
            {
                info.id = "amd_" + std::to_string(deviceIdx);
            }
        }

        // Driver version: ROCm SMI doesn't directly expose driver version
        // We could read from /sys/module/amdgpu/version, but keeping it simple for now
        info.driverVersion = "ROCm";

        gpus.push_back(std::move(info));
    }

    return gpus;
}

std::vector<GPUCounters> ROCmGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    if (!isAvailable())
    {
        return counters;
    }

    counters.reserve(m_Impl->deviceCount);

    for (std::uint32_t deviceIdx = 0; deviceIdx < m_Impl->deviceCount; ++deviceIdx)
    {
        GPUCounters counter{};
        counter.gpuId = std::to_string(deviceIdx);

        // GPU utilization (0-100%)
        std::uint32_t busyPercent = 0;
        rsmi_status_t result = m_Impl->rsmi_dev_gpu_busy_percent_get(deviceIdx, &busyPercent);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(busyPercent);
        }

        // Memory usage (VRAM)
        std::uint64_t memUsed = 0;
        result = m_Impl->rsmi_dev_memory_usage_get(deviceIdx, RSMI_MEM_TYPE_VRAM, &memUsed);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.memoryUsedBytes = memUsed;
        }

        std::uint64_t memTotal = 0;
        result = m_Impl->rsmi_dev_memory_total_get(deviceIdx, RSMI_MEM_TYPE_VRAM, &memTotal);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.memoryTotalBytes = memTotal;
        }

        // Memory utilization percentage is computed by Domain layer from memoryUsedBytes/memoryTotalBytes
        // Platform layer provides raw counters only

        // Temperature (edge/die temperature)
        std::int64_t tempMilliC = 0;
        result = m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_EDGE, RSMI_TEMP_CURRENT, &tempMilliC);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.temperatureC = static_cast<std::int32_t>(tempMilliC / 1000); // Convert milli-degrees to degrees
        }

        // Hotspot temperature (junction temperature)
        std::int64_t hotspotMilliC = 0;
        result = m_Impl->rsmi_dev_temp_metric_get(deviceIdx, RSMI_TEMP_TYPE_JUNCTION, RSMI_TEMP_CURRENT, &hotspotMilliC);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.hotspotTempC = static_cast<std::int32_t>(hotspotMilliC / 1000);
        }
        else
        {
            counter.hotspotTempC = -1; // Not available
        }

        // Power draw (average power in microwatts)
        std::uint64_t powerMicroW = 0;
        result = m_Impl->rsmi_dev_power_ave_get(deviceIdx, 0, &powerMicroW);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMicroW) / 1000000.0; // Convert µW to W
        }

        // Power limit (power cap in microwatts)
        std::uint64_t powerCapMicroW = 0;
        result = m_Impl->rsmi_dev_power_cap_get(deviceIdx, 0, &powerCapMicroW);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerCapMicroW) / 1000000.0; // Convert µW to W
        }

        // GPU clock speed (system clock)
        rsmi_frequencies_t gpuFreq{};
        result = m_Impl->rsmi_dev_gpu_clk_freq_get(deviceIdx, RSMI_CLK_TYPE_SYS, &gpuFreq);
        if (result == RSMI_STATUS_SUCCESS && gpuFreq.current < gpuFreq.num_supported)
        {
            counter.gpuClockMHz = static_cast<std::uint32_t>(gpuFreq.frequency[gpuFreq.current] / 1000000); // Convert Hz to MHz
        }

        // Memory clock speed
        rsmi_frequencies_t memFreq{};
        result = m_Impl->rsmi_dev_gpu_clk_freq_get(deviceIdx, RSMI_CLK_TYPE_MEM, &memFreq);
        if (result == RSMI_STATUS_SUCCESS && memFreq.current < memFreq.num_supported)
        {
            counter.memoryClockMHz = static_cast<std::uint32_t>(memFreq.frequency[memFreq.current] / 1000000); // Convert Hz to MHz
        }

        // Fan speed (sensor 0, in RPM)
        std::int64_t fanSpeed = 0;
        result = m_Impl->rsmi_dev_fan_speed_get(deviceIdx, 0, &fanSpeed);
        if (result == RSMI_STATUS_SUCCESS)
        {
            counter.fanSpeedRPMPercent = static_cast<std::uint32_t>(fanSpeed);
        }

        // PCIe throughput: Not directly available via ROCm SMI
        // Would need to read from sysfs (/sys/class/drm/card*/device/pcie_bw)
        counter.pcieTxBytes = 0;
        counter.pcieRxBytes = 0;

        // Engine utilization: Not available via ROCm SMI
        counter.computeUtilPercent = 0.0;
        counter.encoderUtilPercent = 0.0;
        counter.decoderUtilPercent = 0.0;

        counters.push_back(std::move(counter));
    }

    return counters;
}

std::vector<ProcessGPUCounters> ROCmGPUProbe::readProcessGPUCounters()
{
    // ROCm SMI does not provide per-process GPU utilization or memory allocation
    // This is a known limitation of the ROCm ecosystem compared to NVIDIA's NVML
    // Per-process GPU memory could potentially be obtained from /sys/kernel/debug/dri/[card]/amdgpu_pm_info
    // but this requires root privileges and is not part of the standard ROCm SMI API

    return {};
}

GPUCapabilities ROCmGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!isAvailable())
    {
        return caps;
    }

    // ROCm SMI provides system-level metrics
    caps.hasTemperature = true;
    caps.hasHotspotTemp = true; // Junction temperature available
    caps.hasPowerMetrics = true;
    caps.hasClockSpeeds = true;
    caps.hasFanSpeed = true;
    caps.hasPCIeMetrics = false;       // Not directly available via ROCm SMI
    caps.hasEngineUtilization = false; // Not available
    caps.hasPerProcessMetrics = false; // Major limitation: no per-process data
    caps.hasEncoderDecoder = false;    // Not available via ROCm SMI
    caps.supportsMultiGPU = true;      // Multiple AMD GPUs supported

    return caps;
}

} // namespace Platform
