#include "NVMLGPUProbe.h"

#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>
#include <utility>

// Import NVML types from shared header
using namespace Platform::NVML;

namespace Platform
{

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
    // NVIDIA installs NVML in System32. Restrict the search to that trusted
    // directory so a portable installation cannot load an adjacent DLL.
    m_NVMLHandle = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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
    // Note: SystemGetDriverVersion (not DeviceGet*) - system-wide, not per-device
    m_NVML.SystemGetDriverVersion = reinterpret_cast<decltype(m_NVML.SystemGetDriverVersion)>(
        GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvmlSystemGetDriverVersion"));
    if (m_NVML.SystemGetDriverVersion == nullptr)
    {
        spdlog::warn("NVMLGPUProbe: Failed to load nvmlSystemGetDriverVersion");
        return false;
    }
    LOAD_NVML_FUNC(DeviceGetVbiosVersion)
    LOAD_NVML_FUNC(DeviceGetFanSpeed)

    // Per-process functions (optional - may not be available in older NVML versions)
    // Use a separate macro that doesn't fail on missing functions
#define LOAD_NVML_FUNC_OPTIONAL(name)                                                                                                      \
    m_NVML.name = reinterpret_cast<decltype(m_NVML.name)>(GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvml" #name));               \
    if (m_NVML.name == nullptr)                                                                                                            \
    {                                                                                                                                      \
        spdlog::debug("NVMLGPUProbe: nvml" #name " not available (optional)");                                                             \
    }

    LOAD_NVML_FUNC_OPTIONAL(DeviceGetPcieThroughput)
    LOAD_NVML_FUNC_OPTIONAL(DeviceGetComputeRunningProcesses)
    LOAD_NVML_FUNC_OPTIONAL(DeviceGetGraphicsRunningProcesses)

#undef LOAD_NVML_FUNC_OPTIONAL
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
    result = m_NVML.SystemGetDriverVersion(driverVersion.data(), NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE);
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

std::string NVMLGPUProbe::getNVMLErrorString(NVML::nvmlReturn_t result)
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
        return std::format("Unknown error ({})", static_cast<unsigned int>(result));
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
    std::vector<ProcessGPUCounters> allCounters;

    if (!m_Initialized)
    {
        return allCounters;
    }

    // Check if per-process functions are available
    const bool hasComputeProcs = (m_NVML.DeviceGetComputeRunningProcesses != nullptr);
    const bool hasGraphicsProcs = (m_NVML.DeviceGetGraphicsRunningProcesses != nullptr);

    if (!hasComputeProcs && !hasGraphicsProcs)
    {
        spdlog::debug("NVMLGPUProbe: Per-process GPU functions not available");
        return allCounters;
    }

    // Guards against a buggy/corrupted driver reporting an implausible process count and
    // forcing a huge allocation; no real system runs anywhere near this many GPU contexts.
    constexpr unsigned int kMaxPlausibleProcessCount = 65536;

    for (const auto& [index, device] : m_DeviceHandles)
    {
        // Use index-based GPU ID to match WindowsGPUProbe (DXGI) format
        // The NVML UUID is different from the DXGI LUID-based ID, so we use
        // a consistent index-based format that aligns with the merged snapshots
        std::string gpuId = std::format("GPU{}", index);

        // Query compute processes (CUDA, OpenCL)
        // NVML API pattern: first call with a null buffer returns the required count,
        // then a second call with a buffer sized to that count fetches the entries.
        if (hasComputeProcs)
        {
            unsigned int computeCount = 0;
            nvmlReturn_t result = m_NVML.DeviceGetComputeRunningProcesses(device, &computeCount, nullptr);
            if (computeCount > kMaxPlausibleProcessCount)
            {
                spdlog::warn("NVMLGPUProbe: DeviceGetComputeRunningProcesses reported implausible count {} on GPU {}, skipping",
                             computeCount,
                             index);
                computeCount = 0;
                result = NVML_ERROR_NOT_SUPPORTED;
            }
            std::vector<nvmlProcessInfo_t> computeProcesses;
            if ((result == NVML_SUCCESS || result == NVML_ERROR_INSUFFICIENT_SIZE) && computeCount > 0)
            {
                computeProcesses.resize(computeCount);
                result = m_NVML.DeviceGetComputeRunningProcesses(device, &computeCount, computeProcesses.data());
            }
            if (result == NVML_SUCCESS && computeCount > 0)
            {
                spdlog::debug("NVMLGPUProbe: Found {} compute processes on GPU {}", computeCount, index);
                computeProcesses.resize(computeCount);
                for (const auto& proc : computeProcesses)
                {
                    // NVML uses UINT64_MAX to indicate unavailable memory info
                    constexpr auto NVML_MEMORY_NOT_AVAILABLE = std::numeric_limits<unsigned long long>::max();
                    const bool hasValidMemory = (proc.usedGpuMemory != NVML_MEMORY_NOT_AVAILABLE);

                    ProcessGPUCounters counter;
                    counter.pid = static_cast<std::int32_t>(proc.pid);
                    counter.gpuId = gpuId;
                    counter.gpuMemoryBytes = hasValidMemory ? proc.usedGpuMemory : 0;
                    counter.activeEngines.emplace_back("Compute");
                    spdlog::debug("NVMLGPUProbe: Compute PID {} mem raw={} valid={} final={}",
                                  counter.pid,
                                  proc.usedGpuMemory,
                                  hasValidMemory,
                                  counter.gpuMemoryBytes);
                    allCounters.push_back(std::move(counter));
                }
            }
            else if (result != NVML_SUCCESS && result != NVML_ERROR_NOT_SUPPORTED)
            {
                spdlog::debug("NVMLGPUProbe: DeviceGetComputeRunningProcesses returned {}", static_cast<unsigned int>(result));
            }
        }

        // Query graphics processes (DirectX, OpenGL, Vulkan)
        if (hasGraphicsProcs)
        {
            unsigned int graphicsCount = 0;
            nvmlReturn_t result = m_NVML.DeviceGetGraphicsRunningProcesses(device, &graphicsCount, nullptr);
            if (graphicsCount > kMaxPlausibleProcessCount)
            {
                spdlog::warn("NVMLGPUProbe: DeviceGetGraphicsRunningProcesses reported implausible count {} on GPU {}, skipping",
                             graphicsCount,
                             index);
                graphicsCount = 0;
                result = NVML_ERROR_NOT_SUPPORTED;
            }
            std::vector<nvmlProcessInfo_t> graphicsProcesses;
            if ((result == NVML_SUCCESS || result == NVML_ERROR_INSUFFICIENT_SIZE) && graphicsCount > 0)
            {
                graphicsProcesses.resize(graphicsCount);
                result = m_NVML.DeviceGetGraphicsRunningProcesses(device, &graphicsCount, graphicsProcesses.data());
            }
            if (result == NVML_SUCCESS && graphicsCount > 0)
            {
                spdlog::debug("NVMLGPUProbe: Found {} graphics processes on GPU {}", graphicsCount, index);
                graphicsProcesses.resize(graphicsCount);
                for (const auto& proc : graphicsProcesses)
                {
                    // NVML uses UINT64_MAX to indicate unavailable memory info
                    constexpr auto NVML_MEMORY_NOT_AVAILABLE = std::numeric_limits<unsigned long long>::max();
                    const bool hasValidMemory = (proc.usedGpuMemory != NVML_MEMORY_NOT_AVAILABLE);
                    const std::uint64_t memBytes = hasValidMemory ? proc.usedGpuMemory : 0;

                    // Check if we already have this process from compute list
                    // Note: std::cmp_equal (C++20) handles signedness differences safely
                    auto it = std::ranges::find_if(allCounters,
                                                   [procPid = proc.pid, &gpuId](const ProcessGPUCounters& c)
                                                   { return std::cmp_equal(c.pid, procPid) && c.gpuId == gpuId; });

                    if (it != allCounters.end())
                    {
                        // Merge: add graphics engine, keep max valid memory
                        it->activeEngines.emplace_back("3D");
                        if (hasValidMemory)
                        {
                            it->gpuMemoryBytes = std::max(it->gpuMemoryBytes, memBytes);
                        }
                        spdlog::debug("NVMLGPUProbe: Graphics PID {} (merged) raw={} valid={} final={}",
                                      proc.pid,
                                      proc.usedGpuMemory,
                                      hasValidMemory,
                                      it->gpuMemoryBytes);
                    }
                    else
                    {
                        ProcessGPUCounters counter;
                        counter.pid = static_cast<std::int32_t>(proc.pid);
                        counter.gpuId = gpuId;
                        counter.gpuMemoryBytes = memBytes;
                        counter.activeEngines.emplace_back("3D");
                        spdlog::debug("NVMLGPUProbe: Graphics PID {} (new) raw={} valid={} final={}",
                                      counter.pid,
                                      proc.usedGpuMemory,
                                      hasValidMemory,
                                      counter.gpuMemoryBytes);
                        allCounters.push_back(std::move(counter));
                    }
                }
            }
            else if (result != NVML_SUCCESS && result != NVML_ERROR_NOT_SUPPORTED)
            {
                spdlog::debug("NVMLGPUProbe: DeviceGetGraphicsRunningProcesses returned {}", static_cast<unsigned int>(result));
            }
        }
    }

    spdlog::debug("NVMLGPUProbe: Found {} processes using GPU", allCounters.size());
    return allCounters;
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
    // Per-process metrics available if we have the required functions
    caps.hasPerProcessMetrics = (m_NVML.DeviceGetComputeRunningProcesses != nullptr || m_NVML.DeviceGetGraphicsRunningProcesses != nullptr);
    caps.hasEncoderDecoder = false; // Not implemented yet
    caps.supportsMultiGPU = true;

    return caps;
}

} // namespace Platform
