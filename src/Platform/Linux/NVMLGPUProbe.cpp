#include "NVMLGPUProbe.h"

#include "NVMLGPUProbeMath.h"
#include "PciDisplayDevices.h"
#include "PciRuntimePm.h"
#include "Platform/GPUTypes.h"
#include "Platform/NVMLTypes.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <dlfcn.h>

// Import NVML types from shared header
using namespace Platform::NVML;

namespace Platform
{

struct NVMLGPUProbe::Impl
{
    void* nvmlHandle = nullptr;
    bool initialized = false;
    std::uint32_t deviceCount = 0;

    // Devices whose handle NVML returned, with the id resolved once at load (#1162): the UUID, or
    // "nvidia-<index>" if NVML cannot report one. Re-deriving the id on every read let a transient
    // UUID failure turn a GPU into a phantom "nvidia-N" for one sample.
    struct Device
    {
        nvmlDevice_t handle = nullptr;
        std::uint32_t index = 0;
        std::string id;
        // PCI identity from nvmlDeviceGetPciInfo, when the driver exports it (#1091, #1117).
        std::optional<PciLocation> pciLocation;
        std::uint32_t pciDeviceId = 0;
        // The device's sysfs directory, whose power/runtime_status says whether it is asleep
        // (#1117). Empty when NVML can't report the PCI address: the device is then always queried.
        std::string sysfsPath;
        // The last memory total read while the GPU was awake, reported while it sleeps (#1117).
        std::uint64_t lastMemoryTotalBytes = 0;
        // Read once at load, with the id, so a repeat enumerateGPUs() addresses no sleeping device.
        std::string name;
        // Which sensors this device reports, found by the first enumerateGPUs() that sees it awake
        // (#1112). Unset while it has only been seen asleep: it isn't woken to find out (#1117), and
        // rescanGPUs() asks for a re-enumeration once it is awake (#1289).
        std::optional<GPUCapabilities> sensors;
    };
    std::vector<Device> devices;
    std::string pciDevicesRoot;
    // NVIDIA display devices in sysfs at the last full rescan (PciDisplayDevices::list): a change
    // means a GPU was hot-plugged, removed or rebound, which NVML only sees after a re-init (#1116).
    std::vector<std::string> pciDevicesSeen;
    // A device query returned NVML_ERROR_GPU_IS_LOST or NVML_ERROR_UNINITIALIZED since the last
    // (re)init: NVML has to be re-initialised to talk to the GPUs again (#1116).
    bool gpuLost = false;
    // The last re-init failed (a driver mid-reload, say), so the next full rescan tries again.
    bool restartFailed = false;
    // dlopen() and dlsym() have succeeded (the library stays loaded across a re-init).
    bool symbolsLoaded = false;

    // A running-process entry point and the size of the entries it writes (#1092). The entries are
    // nvmlProcessInfo_v1_t or _v2_t depending on the symbol, so the struct is opaque here: the
    // pointer keeps a struct-pointer parameter like the library's, and the caller's byte buffer is
    // converted only at the call.
    struct ProcessInfoEntries;
    using RunningProcessesFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, ProcessInfoEntries*);
    struct RunningProcessesQuery
    {
        RunningProcessesFn fn = nullptr;
        std::size_t entrySize = 0;
    };
    RunningProcessesQuery computeProcesses;
    RunningProcessesQuery graphicsProcesses;

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
    // Optional: nvml.h maps nvmlDeviceGetPciInfo to the _v3 export; older drivers have only _v2 (same struct).
    nvmlReturn_t (*nvmlDeviceGetPciInfo)(nvmlDevice_t, nvmlPciInfo_t*) = nullptr;
    const char* (*nvmlErrorString)(nvmlReturn_t) = nullptr;

    /// Whether the device is runtime-suspended now, so must not be queried (#1117).
    [[nodiscard]] static bool asleep(const Device& device)
    {
        return PciRuntimePm::isRuntimeSuspended(device.sysfsPath);
    }

    /// Note a device query's result: a lost GPU or an uninitialised library means NVML must be
    /// re-initialised (#1116). Returns `result` unchanged.
    nvmlReturn_t noteResult(nvmlReturn_t result)
    {
        if (result == NVML_ERROR_GPU_IS_LOST || result == NVML_ERROR_UNINITIALIZED)
        {
            gpuLost = true;
        }
        return result;
    }

    bool loadNVML();
    bool loadSymbols();
    /// nvmlInit_v2() and the device list. On failure NVML is shut down again and false returned.
    bool startNVML();
    /// Re-initialise NVML and rebuild the device list (#1116), keeping each surviving device's
    /// last-known memory total. NVML is left unavailable if the re-init fails.
    void restartNVML();
    [[nodiscard]] RunningProcessesQuery loadRunningProcessesQuery(const std::string& baseName) const;
    void unloadNVML();
    [[nodiscard]] std::string getNVMLError(nvmlReturn_t result) const;
};

bool NVMLGPUProbe::Impl::loadNVML()
{
    if (initialized)
    {
        return true;
    }
    if (!symbolsLoaded && !loadSymbols())
    {
        return false;
    }
    if (!startNVML())
    {
        unloadNVML();
        return false;
    }
    return true;
}

bool NVMLGPUProbe::Impl::loadSymbols()
{
    // Try to load libnvidia-ml.so (dynamic loading for graceful fallback)
    // Note: tests inject the mock by setting LD_LIBRARY_PATH before process start (via CTest ENVIRONMENT_MODIFICATION)
    if (nvmlHandle == nullptr)
    {
        nvmlHandle = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    }
    if (nvmlHandle == nullptr)
    {
        nvmlHandle = dlopen("libnvidia-ml.so", RTLD_NOW);
        if (nvmlHandle == nullptr)
        {
            // NOLINTNEXTLINE(concurrency-mt-unsafe) - dlerror() is called during single-threaded initialization
            spdlog::debug("NVMLGPUProbe: Failed to load libnvidia-ml.so - {}", dlerror());
            return false;
        }
    }

// Load function pointers
// NOLINTBEGIN(bugprone-macro-parentheses) - 'name' is used as identifier and stringified, cannot be parenthesized
#define LOAD_NVML_FUNC(name)                                                                                                               \
    name = reinterpret_cast<decltype(name)>(dlsym(nvmlHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::error("NVMLGPUProbe: Failed to load symbol " #name);                                                                       \
        unloadNVML();                                                                                                                      \
        return false;                                                                                                                      \
    }

    // Optional: not required for GPU enumeration/monitoring to function. A missing symbol is
    // logged and left null; callers must null-check before use.
#define LOAD_NVML_FUNC_OPTIONAL(name)                                                                                                      \
    name = reinterpret_cast<decltype(name)>(dlsym(nvmlHandle, #name));                                                                     \
    if (name == nullptr)                                                                                                                   \
    {                                                                                                                                      \
        spdlog::debug("NVMLGPUProbe: optional symbol " #name " not available");                                                            \
    }
    // NOLINTEND(bugprone-macro-parentheses)

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
    // Loaded but not called today (readGPUCounters() explains why, further down) - kept
    // optional so a minimal/older NVML build missing it doesn't block loading the rest of
    // the counters.
    LOAD_NVML_FUNC_OPTIONAL(nvmlDeviceGetPcieThroughput);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) - dlsym returns void* by POSIX definition
    nvmlDeviceGetPciInfo = reinterpret_cast<decltype(nvmlDeviceGetPciInfo)>(dlsym(nvmlHandle, "nvmlDeviceGetPciInfo_v3"));
    if (nvmlDeviceGetPciInfo == nullptr)
    {
        nvmlDeviceGetPciInfo = reinterpret_cast<decltype(nvmlDeviceGetPciInfo)>(dlsym(nvmlHandle, "nvmlDeviceGetPciInfo_v2"));
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    computeProcesses = loadRunningProcessesQuery("nvmlDeviceGetComputeRunningProcesses");
    graphicsProcesses = loadRunningProcessesQuery("nvmlDeviceGetGraphicsRunningProcesses");
    if (computeProcesses.fn == nullptr || graphicsProcesses.fn == nullptr)
    {
        spdlog::error("NVMLGPUProbe: Failed to load the running-process symbols");
        unloadNVML();
        return false;
    }
    LOAD_NVML_FUNC(nvmlErrorString);

#undef LOAD_NVML_FUNC_OPTIONAL
#undef LOAD_NVML_FUNC

    symbolsLoaded = true;
    return true;
}

bool NVMLGPUProbe::Impl::startNVML()
{
    // Initialize NVML
    auto result = nvmlInit_v2();
    if (result != NVML_SUCCESS)
    {
        spdlog::error("NVMLGPUProbe: nvmlInit_v2 failed - {}", getNVMLError(result));
        return false;
    }

    // Get device count
    result = nvmlDeviceGetCount_v2(&deviceCount);
    if (result != NVML_SUCCESS)
    {
        spdlog::error("NVMLGPUProbe: nvmlDeviceGetCount_v2 failed - {}", getNVMLError(result));
        nvmlShutdown();
        deviceCount = 0;
        return false;
    }

    // Get device handles. A device whose handle NVML won't return is skipped rather than sampled
    // through a null handle, which produced an all-zero phantom GPU (#1162).
    devices.clear();
    devices.reserve(deviceCount);
    for (std::uint32_t i = 0; i < deviceCount; ++i)
    {
        nvmlDevice_t handle = nullptr;
        result = nvmlDeviceGetHandleByIndex_v2(i, &handle);
        if (result != NVML_SUCCESS || handle == nullptr)
        {
            spdlog::warn("NVMLGPUProbe: Failed to get handle for GPU {} - {}", i, getNVMLError(result));
            continue;
        }

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - C API buffer
        char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
        std::string id =
            (nvmlDeviceGetUUID(handle, uuid, sizeof(uuid)) == NVML_SUCCESS) ? std::string(uuid) : "nvidia-" + std::to_string(i);
        Device device;
        device.handle = handle;
        device.index = i;
        device.id = std::move(id);
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - C API buffer
        char name[NVML_DEVICE_NAME_BUFFER_SIZE]{};
        if (nvmlDeviceGetName(handle, name, sizeof(name)) == NVML_SUCCESS)
        {
            device.name = name;
        }

        // PCI identity, read once: it says where the device's power/runtime_status is (#1117).
        nvmlPciInfo_t pci{};
        if (nvmlDeviceGetPciInfo != nullptr && nvmlDeviceGetPciInfo(handle, &pci) == NVML_SUCCESS)
        {
            device.pciLocation = PciLocation{.bus = pci.bus, .device = pci.device};
            device.pciDeviceId = pci.pciDeviceId;
            device.sysfsPath = pciDevicesRoot + "/" + NVMLGPUProbeMath::sysfsPciAddress(pci);
        }
        devices.push_back(std::move(device));
    }

    initialized = true;
    gpuLost = false;
    spdlog::info("NVMLGPUProbe: Initialized successfully, found {} NVIDIA GPU(s)", deviceCount);
    return true;
}

void NVMLGPUProbe::Impl::restartNVML()
{
    std::unordered_map<std::string, std::uint64_t> lastMemoryTotals;
    for (const auto& device : devices)
    {
        lastMemoryTotals.emplace(device.id, device.lastMemoryTotalBytes);
    }

    // nvmlShutdown() then nvmlInit_v2() is NVML's supported way to start over; the library stays
    // loaded unless the re-init fails (loadNVML() then unloads it, and the next restart reloads it).
    if (initialized)
    {
        nvmlShutdown();
        initialized = false;
    }
    devices.clear();
    deviceCount = 0;
    gpuLost = false;
    restartFailed = !loadNVML();
    if (restartFailed)
    {
        spdlog::warn("NVMLGPUProbe: NVML re-initialisation failed; retrying at the next full rescan");
        return;
    }
    for (auto& device : devices)
    {
        if (const auto it = lastMemoryTotals.find(device.id); it != lastMemoryTotals.end())
        {
            device.lastMemoryTotalBytes = it->second;
        }
    }
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
    symbolsLoaded = false;
    deviceCount = 0;
    devices.clear();
}

NVMLGPUProbe::Impl::RunningProcessesQuery NVMLGPUProbe::Impl::loadRunningProcessesQuery(const std::string& baseName) const
{
    const auto symbol = NVMLGPUProbeMath::chooseRunningProcessesSymbol(
        baseName, [this](const std::string& name) { return dlsym(nvmlHandle, name.c_str()); });
    if (symbol.address == nullptr)
    {
        return {};
    }
    spdlog::debug("NVMLGPUProbe: using {} ({}-byte entries)", symbol.name, symbol.entrySize);
    // dlsym returns void* by POSIX definition; the cast to the known NVML signature is required.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {.fn = reinterpret_cast<RunningProcessesFn>(symbol.address), .entrySize = symbol.entrySize};
}

std::string NVMLGPUProbe::Impl::getNVMLError(nvmlReturn_t result) const
{
    return NVMLGPUProbeMath::resolveErrorString(result, nvmlErrorString);
}

// Constructor
NVMLGPUProbe::NVMLGPUProbe(std::string pciDevicesRoot) : m_Impl(std::make_unique<Impl>())
{
    m_Impl->pciDevicesRoot = std::move(pciDevicesRoot);
    m_Impl->pciDevicesSeen = PciDisplayDevices::list(m_Impl->pciDevicesRoot, PciDisplayDevices::PCI_VENDOR_NVIDIA);
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
    gpus.reserve(m_Impl->devices.size());

    for (auto& dev : m_Impl->devices)
    {
        nvmlDevice_t device = dev.handle;

        GPUInfo info;
        info.deviceIndex = dev.index;
        info.vendor = "NVIDIA";
        info.isIntegrated = false; // NVIDIA GPUs are typically discrete
        info.name = dev.name;      // Read at load, so a repeat enumeration addresses no device
        info.id = dev.id;
        info.pciLocation = dev.pciLocation;
        info.pciDeviceId = dev.pciDeviceId;

        // Which sensors this device actually reports: capabilities() covers NVML as a whole, but a
        // passively cooled card has no fan reading and a laptop GPU may not report power, as on
        // Windows (#1040, #1112). Only NVML_ERROR_NOT_SUPPORTED means the device lacks a sensor: a
        // transient failure now (a timeout, a busy GPU) must not hide it for the session, since the
        // answer is kept; its readings are just unavailable until a read succeeds (#1111). Found
        // once per device: a sleeping GPU isn't woken to find out (#1117), so until it is seen awake
        // the probe's capabilities apply to it, and rescanGPUs() asks for a re-enumeration then (#1289).
        if (!dev.sensors.has_value() && !Impl::asleep(dev))
        {
            const auto supported = [](nvmlReturn_t result)
            {
                return result != NVML_ERROR_NOT_SUPPORTED;
            };
            unsigned int probeValue = 0;
            GPUCapabilities sensors = capabilities();
            sensors.hasTemperature = supported(m_Impl->nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &probeValue));
            sensors.hasPowerMetrics = supported(m_Impl->nvmlDeviceGetPowerUsage(device, &probeValue));
            sensors.hasClockSpeeds = supported(m_Impl->nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &probeValue));
            sensors.hasFanSpeed = supported(m_Impl->nvmlDeviceGetFanSpeed(device, &probeValue));
            dev.sensors = sensors;
        }
        info.sensorCapabilities = dev.sensors;

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
    counters.reserve(m_Impl->devices.size());

    for (auto& dev : m_Impl->devices)
    {
        nvmlDevice_t device = dev.handle;
        GPUCounters counter;
        counter.gpuId = dev.id;

        // A runtime-suspended GPU gets no NVML query at all, which would wake it (#1117): every
        // reading is unavailable this sample, and the memory total is the last one read awake.
        if (Impl::asleep(dev))
        {
            counter.suspended = true;
            counter.utilizationAvailable = false;
            counter.temperatureAvailable = false;
            counter.powerAvailable = false;
            counter.gpuClockAvailable = false;
            counter.memoryAvailable = false;
            counter.memoryTotalBytes = dev.lastMemoryTotalBytes;
            counters.push_back(std::move(counter));
            continue;
        }

        // Memory info
        nvmlMemory_t memInfo{};
        auto result = m_Impl->noteResult(m_Impl->nvmlDeviceGetMemoryInfo(device, &memInfo));
        if (result == NVML_SUCCESS)
        {
            counter.memoryUsedBytes = memInfo.used;
            counter.memoryTotalBytes = memInfo.total;
            dev.lastMemoryTotalBytes = memInfo.total;
            // Note: memoryUtilPercent is computed in Domain layer from raw bytes
        }
        else
        {
            counter.memoryAvailable = false; // Unread this sample: not a real 0% (#1111)
        }

        // Utilization
        nvmlUtilization_t util{};
        result = m_Impl->noteResult(m_Impl->nvmlDeviceGetUtilizationRates(device, &util));
        if (result == NVML_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(util.gpu);
        }
        else
        {
            counter.utilizationAvailable = false; // Unread this sample: not a real 0% (#1111)
        }

        // Temperature
        unsigned int temp = 0;
        result = m_Impl->noteResult(m_Impl->nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp));
        if (result == NVML_SUCCESS)
        {
            counter.temperatureC = static_cast<std::int32_t>(temp);
        }
        else
        {
            counter.temperatureAvailable = false;
        }

        // Power
        unsigned int powerMilliwatts = 0;
        result = m_Impl->noteResult(m_Impl->nvmlDeviceGetPowerUsage(device, &powerMilliwatts));
        if (result == NVML_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMilliwatts) / 1000.0;
        }
        else
        {
            counter.powerAvailable = false;
        }

        unsigned int powerLimitMilliwatts = 0;
        result = m_Impl->nvmlDeviceGetPowerManagementLimit(device, &powerLimitMilliwatts);
        if (result == NVML_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerLimitMilliwatts) / 1000.0;
        }

        // Clock speeds
        unsigned int gpuClock = 0;
        result = m_Impl->noteResult(m_Impl->nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &gpuClock));
        if (result == NVML_SUCCESS)
        {
            counter.gpuClockMHz = gpuClock;
        }
        else
        {
            counter.gpuClockAvailable = false;
        }

        unsigned int memClock = 0;
        result = m_Impl->nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &memClock);
        if (result == NVML_SUCCESS)
        {
            counter.memoryClockMHz = memClock;
        }

        // Fan speed: nvmlDeviceGetFanSpeed() already returns a 0-100 percentage, so the max is
        // always 100 (see GPUCounters::fanSpeedRaw/fanSpeedMaxRaw; Domain computes the percent).
        unsigned int fanSpeed = 0;
        result = m_Impl->nvmlDeviceGetFanSpeed(device, &fanSpeed);
        if (result == NVML_SUCCESS)
        {
            counter.fanSpeedRaw = fanSpeed;
            counter.fanSpeedMaxRaw = 100;
        }

        // PCIe throughput: NVML returns rates (KB/s over a ~20ms sampling window), not
        // cumulative counters. GPUTypes.h expects cumulative pcieTxBytes/pcieRxBytes, and
        // GPUModel diffs consecutive samples via Numeric::counterRate() to derive a rate,
        // which clamps to 0 whenever a fluctuating rate-of-rate sample decreases between
        // reads - so populating these from nvmlDeviceGetPcieThroughput() would silently
        // corrupt PCIe throughput reporting rather than just leaving it unavailable. Mirrors
        // the same decision already made in the Windows NVMLGPUProbe.
        // Since NVML doesn't provide cumulative counters, we leave these at 0.
        // Future enhancement: Add rate fields or implement tracking.
        // For now, Domain layer will compute rates as 0 from cumulative fields.

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

    const auto runningProcesses = [](const Impl::RunningProcessesQuery& query, nvmlDevice_t device)
    {
        return NVMLGPUProbeMath::queryRunningProcesses([&query, device](unsigned int* count, void* buffer)
                                                       { return query.fn(device, count, static_cast<Impl::ProcessInfoEntries*>(buffer)); },
                                                       query.entrySize);
    };

    for (const auto& dev : m_Impl->devices)
    {
        // A sleeping GPU runs no processes, and asking would wake it (#1117).
        if (Impl::asleep(dev))
        {
            continue;
        }
        // One row per process on this device, combined from both lists (instances under MIG summed).
        for (const auto& usage : NVMLGPUProbeMath::combineRunningProcesses(runningProcesses(m_Impl->computeProcesses, dev.handle),
                                                                           runningProcesses(m_Impl->graphicsProcesses, dev.handle)))
        {
            ProcessGPUCounters counter;
            counter.pid = static_cast<std::int32_t>(usage.pid);
            counter.gpuId = dev.id;
            counter.gpuMemoryBytes = usage.memoryBytes;
            if (usage.compute)
            {
                counter.activeEngines.emplace_back("Compute");
            }
            if (usage.graphics)
            {
                counter.activeEngines.emplace_back("3D");
            }
            allCounters.push_back(std::move(counter));
        }
    }

    return allCounters;
}

bool NVMLGPUProbe::rescanGPUs(GPURescan depth)
{
    if (depth == GPURescan::Full)
    {
        auto seen = PciDisplayDevices::list(m_Impl->pciDevicesRoot, PciDisplayDevices::PCI_VENDOR_NVIDIA);
        const bool pciChanged = seen != m_Impl->pciDevicesSeen;
        m_Impl->pciDevicesSeen = std::move(seen);
        // A lost GPU waits for a full rescan rather than re-initialising NVML on the next sample, so
        // a GPU that stays lost costs one re-init per interval, not one per sample.
        if (pciChanged || m_Impl->gpuLost || m_Impl->restartFailed)
        {
            const char* reason = "retrying a failed re-init";
            if (pciChanged)
            {
                reason = "NVIDIA PCI devices changed";
            }
            else if (m_Impl->gpuLost)
            {
                reason = "a GPU was lost";
            }
            spdlog::info("NVMLGPUProbe: {}; re-initialising NVML", reason);
            m_Impl->restartNVML();
            return true;
        }
    }

    // An adapter asleep when it was enumerated and awake now: enumerate again to find its sensors
    // (#1289). Reads only runtime_status, and only for such adapters.
    return isAvailable() &&
           std::ranges::any_of(m_Impl->devices,
                               [](const Impl::Device& device) { return !device.sensors.has_value() && !Impl::asleep(device); });
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
        // NVML only returns PCIe throughput as rates, not cumulative counters, so
        // pcieTxBytes/pcieRxBytes are deliberately left at 0 (see the comment where counters
        // are populated above) -- report the capability as unavailable, not present-but-zero.
        caps.hasPCIeMetrics = false;
        caps.hasPerProcessMetrics = true;
        caps.supportsMultiGPU = true;
        caps.hasEngineUtilization = true; // Via activeEngines in ProcessGPUCounters
    }

    return caps;
}

} // namespace Platform
