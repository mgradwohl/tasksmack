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
#include <filesystem>
#include <fstream>
#include <iterator>
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
        // Null while the device is deferred: it was asleep when NVML started, so it hasn't been asked
        // for a handle, which would wake it (#1270). Its id, name and PCI identity then come from
        // sysfs and the driver's procfs, and resolveDeferred() looks it up by PCI address once awake.
        nvmlDevice_t handle = nullptr;
        std::uint32_t index = 0;
        std::string id;
        // The id is the GPU's UUID (from NVML, or from the driver's procfs while deferred), not the
        // "nvidia-<...>" fallback.
        bool idIsUuid = false;
        // The sysfs name ("0000:01:00.0"), when known; how a deferred device is looked up.
        std::string pciAddress;
        // A deferred device whose lookup failed while it was awake -- once it woke, or at the start
        // itself for an awake GPU listed alongside a sleeping one -- isn't retried until the next full
        // rescan clears this (or NVML restarts), so a lookup that keeps failing asks for one
        // re-enumeration per full-rescan interval rather than one every sample.
        bool resolveFailed = false;
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
    // Where the nvidia driver lists its GPUs by PCI address (/proc/driver/nvidia/gpus), each with an
    // "information" file that is read without waking the GPU (#1270).
    std::string nvidiaProcRoot;
    // NVIDIA display devices in sysfs at the last full rescan (PciDisplayDevices::list): a change
    // means a GPU was hot-plugged, removed or rebound, which NVML only sees after a re-init (#1116).
    std::vector<std::string> pciDevicesSeen;
    // A device query returned NVML_ERROR_GPU_IS_LOST or NVML_ERROR_UNINITIALIZED since the last
    // (re)init: NVML has to be re-initialised to talk to the GPUs again (#1116).
    bool gpuLost = false;
    // The last load or re-init failed while an nvidia-bound GPU is present (a driver mid-reload,
    // say), so the next full rescan tries again (#1116).
    bool loadRetryPending = false;
    // dlopen() and dlsym() have succeeded (the library stays loaded across a re-init).
    bool symbolsLoaded = false;
    // dlopen() and dlsym() have succeeded at least once: NVML is installed, so a failed start is the
    // driver not being ready rather than NVML missing, and is worth retrying.
    bool libraryFound = false;

    // A running-process entry point and the size of the entries it writes (#1092). The entries are
    // nvmlProcessInfo_v1_t or _v2_t depending on the symbol, so the struct is opaque here: the
    // pointer keeps a struct-pointer parameter like the library's, and the caller's byte buffer is
    // converted only at the call. The opaque type is the shared NVML::nvmlProcessInfoEntries, which
    // the test mock's definitions also take, so the call goes through the callee's own function
    // type (#1306; UBSan -fsanitize=function).
    using RunningProcessesFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, nvmlProcessInfoEntries*);
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
    // Optional: a device asleep when NVML starts is deferred only when both exist (#1270).
    nvmlReturn_t (*nvmlDeviceGetHandleByPciBusId_v2)(const char*, nvmlDevice_t*) = nullptr;
    nvmlReturn_t (*nvmlDeviceGetIndex)(nvmlDevice_t, unsigned int*) = nullptr;
    const char* (*nvmlErrorString)(nvmlReturn_t) = nullptr;

    /// Whether the device is runtime-suspended now, so must not be queried (#1117).
    [[nodiscard]] static bool asleep(const Device& device)
    {
        return PciRuntimePm::isRuntimeSuspended(device.sysfsPath);
    }

    /// Whether the device still has something to learn once awake: its handle (deferred, #1270) or
    /// its own sensor set (#1289).
    [[nodiscard]] static bool awaitingWake(const Device& device)
    {
        if (device.handle == nullptr)
        {
            return !device.resolveFailed; // its sensors can only be found through a handle
        }
        return !device.sensors.has_value();
    }

    /// A device NVML returned a handle for, described by NVML: UUID (else "nvidia-<index>"), name
    /// and PCI identity, read once (#1162). These are the device-addressed calls that would wake a
    /// runtime-suspended GPU, so a deferred device gets them only once awake (#1270).
    [[nodiscard]] Device describe(nvmlDevice_t handle, std::uint32_t index) const;
    /// A device asleep when NVML started, described without NVML (#1270): the PCI location from its
    /// address, the device id from sysfs, the model and UUID from the driver's procfs.
    [[nodiscard]] Device describeDeferred(const std::string& address, std::uint32_t provisionalIndex) const;
    /// Builds the device list. Defers the devices asleep now when that is possible: the PCI bus-id
    /// lookup is available and sysfs lists exactly as many nvidia-bound devices as NVML counts, so
    /// every NVML device can be found by address. Returns false, building nothing, otherwise.
    bool buildDeviceListDeferringSleepers();
    /// Looks a deferred device up by PCI address, now that it is awake, and describes it from NVML,
    /// keeping what was learnt meanwhile. False if NVML won't return its handle.
    bool resolveDeferred(Device& device);

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
    /// Whether a failed load or re-init is retried at the next full rescan: NVML is installed and a
    /// GPU bound to the nvidia driver is present, so it should come up once the driver is ready.
    /// Otherwise (no NVIDIA GPU, or one on nouveau or vfio-pci) retrying could never succeed; a
    /// driver binding later changes the PCI list, which restarts NVML anyway.
    [[nodiscard]] bool loadFailureIsRetryable() const
    {
        return libraryFound && PciDisplayDevices::anyBoundTo(pciDevicesSeen, PciDisplayDevices::DRIVER_NVIDIA);
    }
    bool loadSymbols();
    /// nvmlInit_v2() and the device list. On failure NVML is shut down again and false returned.
    bool startNVML();
    /// Re-initialise NVML and rebuild the device list (#1116), keeping each surviving device's
    /// last-known memory total and the sensor set already found for it, so one asleep through the
    /// restart doesn't lose it (it isn't woken to find it again, #1117). A device reporting no UUID
    /// keeps the one known at its PCI address; one reporting a different UUID is a different GPU and
    /// inherits nothing (#1270). NVML is left unavailable if the re-init fails.
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
    // Without these a GPU asleep at start-up is looked up by index like the rest, which wakes it
    // once (#1270).
    LOAD_NVML_FUNC_OPTIONAL(nvmlDeviceGetHandleByPciBusId_v2);
    LOAD_NVML_FUNC_OPTIONAL(nvmlDeviceGetIndex);
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
    libraryFound = true;
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
    if (!buildDeviceListDeferringSleepers())
    {
        for (std::uint32_t i = 0; i < deviceCount; ++i)
        {
            nvmlDevice_t handle = nullptr;
            result = nvmlDeviceGetHandleByIndex_v2(i, &handle);
            if (result != NVML_SUCCESS || handle == nullptr)
            {
                spdlog::warn("NVMLGPUProbe: Failed to get handle for GPU {} - {}", i, getNVMLError(result));
                continue;
            }
            devices.push_back(describe(handle, i));
        }
    }

    initialized = true;
    gpuLost = false;
    spdlog::info("NVMLGPUProbe: Initialized successfully, found {} NVIDIA GPU(s)", deviceCount);
    return true;
}

NVMLGPUProbe::Impl::Device NVMLGPUProbe::Impl::describe(nvmlDevice_t handle, std::uint32_t index) const
{
    Device device;
    device.handle = handle;
    device.index = index;
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - C API buffer
    char uuid[NVML_DEVICE_UUID_BUFFER_SIZE]{};
    device.idIsUuid = nvmlDeviceGetUUID(handle, uuid, sizeof(uuid)) == NVML_SUCCESS;
    device.id = device.idIsUuid ? std::string(uuid) : "nvidia-" + std::to_string(index);
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
        device.pciAddress = NVMLGPUProbeMath::sysfsPciAddress(pci);
        device.sysfsPath = pciDevicesRoot + "/" + device.pciAddress;
    }
    return device;
}

NVMLGPUProbe::Impl::Device NVMLGPUProbe::Impl::describeDeferred(const std::string& address, std::uint32_t provisionalIndex) const
{
    Device device;
    device.index = provisionalIndex;
    device.pciAddress = address;
    device.sysfsPath = pciDevicesRoot + "/" + address;
    if (const auto fields = NVMLGPUProbeMath::parsePciAddress(address))
    {
        device.pciLocation = PciLocation{.bus = fields->bus, .device = fields->device};
    }
    // sysfs caches the PCI ids; NVML encodes pciDeviceId as (device id << 16) | vendor id.
    std::uint32_t deviceId = 0;
    if (PciDisplayDevices::readHexAttribute(std::filesystem::path(device.sysfsPath) / "device", deviceId))
    {
        constexpr unsigned DEVICE_ID_SHIFT = 16U;
        device.pciDeviceId = (deviceId << DEVICE_ID_SHIFT) | PciDisplayDevices::PCI_VENDOR_NVIDIA;
    }

    NVMLGPUProbeMath::NvidiaProcGpuInfo procInfo;
    if (std::ifstream file(nvidiaProcRoot + "/" + address + "/information"); file.is_open())
    {
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        procInfo = NVMLGPUProbeMath::parseNvidiaProcGpuInformation(text);
    }
    device.idIsUuid = !procInfo.uuid.empty();
    // Without the UUID the id is the PCI address, which is stable while the GPU stays in its slot.
    // NVML's UUID replaces it once the GPU wakes, so its history then starts afresh.
    device.id = device.idIsUuid ? procInfo.uuid : "nvidia-" + address;
    device.name = procInfo.model.empty() ? std::string("NVIDIA GPU") : procInfo.model;
    return device;
}

bool NVMLGPUProbe::Impl::buildDeviceListDeferringSleepers()
{
    if (nvmlDeviceGetHandleByPciBusId_v2 == nullptr || nvmlDeviceGetIndex == nullptr)
    {
        return false;
    }
    // pciDevicesSeen is current: the constructor and every full rescan list it before (re)starting NVML.
    const auto addresses = PciDisplayDevices::addressesBoundTo(pciDevicesSeen, PciDisplayDevices::DRIVER_NVIDIA);
    if (addresses.size() != deviceCount ||
        std::ranges::none_of(
            addresses, [this](const std::string& address) { return PciRuntimePm::isRuntimeSuspended(pciDevicesRoot + "/" + address); }))
    {
        // Nothing asleep, or sysfs doesn't account for every NVML device (WSL has no PCI sysfs, a
        // container may hide it): enumerate by index as always.
        return false;
    }

    for (std::uint32_t position = 0; position < addresses.size(); ++position)
    {
        const auto& address = addresses[position];
        if (PciRuntimePm::isRuntimeSuspended(pciDevicesRoot + "/" + address))
        {
            // NVML numbers devices in PCI order in practice; the real index is read once it wakes.
            devices.push_back(describeDeferred(address, position));
            spdlog::info("NVMLGPUProbe: GPU at {} is runtime-suspended; deferring its NVML queries until it wakes", address);
            continue;
        }
        nvmlDevice_t handle = nullptr;
        const auto result = nvmlDeviceGetHandleByPciBusId_v2(address.c_str(), &handle);
        if (result != NVML_SUCCESS || handle == nullptr)
        {
            // Kept, handle-less, as a failed deferred lookup rather than dropped: this path returns
            // success, so nothing else enumerates it, and with no PCI change or lost GPU no restart
            // follows either. The next full rescan retries the lookup (#1270 review).
            spdlog::warn(
                "NVMLGPUProbe: Failed to get handle for GPU at {} - {}; retrying at the next full rescan", address, getNVMLError(result));
            Device device = describeDeferred(address, position);
            device.resolveFailed = true;
            devices.push_back(std::move(device));
            continue;
        }
        unsigned int index = position;
        if (nvmlDeviceGetIndex(handle, &index) != NVML_SUCCESS)
        {
            index = position;
        }
        devices.push_back(describe(handle, index));
    }
    return true;
}

bool NVMLGPUProbe::Impl::resolveDeferred(Device& device)
{
    nvmlDevice_t handle = nullptr;
    const auto result = noteResult(nvmlDeviceGetHandleByPciBusId_v2(device.pciAddress.c_str(), &handle));
    if (result != NVML_SUCCESS || handle == nullptr)
    {
        spdlog::warn("NVMLGPUProbe: Failed to get handle for woken GPU at {} - {}", device.pciAddress, getNVMLError(result));
        device.resolveFailed = true;
        return false;
    }
    unsigned int index = device.index;
    if (nvmlDeviceGetIndex(handle, &index) != NVML_SUCCESS)
    {
        index = device.index;
    }
    Device resolved = describe(handle, index);
    if (!resolved.idIsUuid && device.idIsUuid)
    {
        resolved.id = device.id; // NVML couldn't report the UUID the driver already gave
        resolved.idIsUuid = true;
    }
    if (resolved.name.empty())
    {
        resolved.name = device.name;
    }
    if (resolved.sysfsPath.empty())
    {
        resolved.pciAddress = device.pciAddress;
        resolved.sysfsPath = device.sysfsPath;
        resolved.pciLocation = device.pciLocation;
        resolved.pciDeviceId = device.pciDeviceId;
    }
    resolved.lastMemoryTotalBytes = device.lastMemoryTotalBytes;
    resolved.sensors = device.sensors;
    device = std::move(resolved);
    return true;
}

void NVMLGPUProbe::Impl::restartNVML()
{
    // What each device learnt while it was known, by id: a GPU suspended now can't be asked again.
    struct Remembered
    {
        std::uint64_t lastMemoryTotalBytes = 0;
        std::optional<GPUCapabilities> sensors;
    };
    std::unordered_map<std::string, Remembered> remembered;
    // The identity each PCI address had, so a rebuilt device that reports no UUID -- deferred
    // without one in procfs (#1270), or awake with NVML's UUID query failing -- keeps its id, and so
    // its history, rather than taking an index- or address-based one.
    std::unordered_map<std::string, Device> rememberedByAddress;
    for (const auto& device : devices)
    {
        remembered.emplace(device.id, Remembered{.lastMemoryTotalBytes = device.lastMemoryTotalBytes, .sensors = device.sensors});
        if (!device.pciAddress.empty())
        {
            rememberedByAddress.emplace(device.pciAddress, device);
        }
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
    if (!loadNVML())
    {
        loadRetryPending = loadFailureIsRetryable();
        spdlog::warn("NVMLGPUProbe: NVML re-initialisation failed{}", loadRetryPending ? "; retrying at the next full rescan" : "");
        return;
    }
    loadRetryPending = false;
    for (auto& device : devices)
    {
        if (const auto known = rememberedByAddress.find(device.pciAddress); known != rememberedByAddress.end())
        {
            const Device& prior = known->second;
            if (NVMLGPUProbeMath::keepsRememberedId(device.idIsUuid, device.handle != nullptr, prior.idIsUuid))
            {
                device.id = prior.id;
                device.idIsUuid = prior.idIsUuid;
            }
            else if (device.idIsUuid && prior.idIsUuid && device.id != prior.id)
            {
                spdlog::info("NVMLGPUProbe: a different GPU ({}) is now at {}; not carrying over what was known of {}",
                             device.id,
                             device.pciAddress,
                             prior.id);
            }
            // A deferred device that is the same GPU keeps the NVML description it had over the
            // provisional one from sysfs and procfs.
            if (device.handle == nullptr && device.id == prior.id)
            {
                device.name = prior.name;
                device.index = prior.index;
                device.pciDeviceId = prior.pciDeviceId;
            }
        }
        // By id: a different GPU at a known address (a new UUID) gets nothing of the old one's.
        if (const auto it = remembered.find(device.id); it != remembered.end())
        {
            device.lastMemoryTotalBytes = it->second.lastMemoryTotalBytes;
            device.sensors = it->second.sensors;
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
NVMLGPUProbe::NVMLGPUProbe(std::string pciDevicesRoot, std::string nvidiaProcRoot) : m_Impl(std::make_unique<Impl>())
{
    m_Impl->pciDevicesRoot = std::move(pciDevicesRoot);
    m_Impl->nvidiaProcRoot = std::move(nvidiaProcRoot);
    m_Impl->pciDevicesSeen = PciDisplayDevices::list(m_Impl->pciDevicesRoot, PciDisplayDevices::PCI_VENDOR_NVIDIA);
    // NVML installed but not starting while an nvidia-bound GPU is present (TaskSmack started during
    // a driver reload, say) is retried at the next full rescan, which reports the GPUs it then finds.
    if (!m_Impl->loadNVML() && m_Impl->loadFailureIsRetryable())
    {
        m_Impl->loadRetryPending = true;
        spdlog::info("NVMLGPUProbe: NVML did not start with an NVIDIA GPU present; retrying at the next full rescan");
    }
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
        // A device deferred because it was asleep when NVML started is looked up now that it is
        // awake (rescanGPUs() asks for this enumeration once it wakes, #1270).
        if (dev.handle == nullptr && !dev.resolveFailed && !Impl::asleep(dev))
        {
            m_Impl->resolveDeferred(dev); // on failure it stays unread until the next full rescan retries it
        }
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
        if (device != nullptr && !dev.sensors.has_value() && !Impl::asleep(dev))
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
        // reading is unavailable this sample, and the memory total is the last one read awake. A
        // deferred device (no handle yet, #1270) that has just woken is likewise unread until the
        // re-enumeration rescanGPUs() asks for looks it up.
        const bool asleep = Impl::asleep(dev);
        if (asleep || device == nullptr)
        {
            counter.suspended = asleep;
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
                                                       { return query.fn(device, count, static_cast<nvmlProcessInfoEntries*>(buffer)); },
                                                       query.entrySize);
    };

    for (const auto& dev : m_Impl->devices)
    {
        // A sleeping GPU runs no processes, and asking would wake it (#1117); a deferred one has no
        // handle to ask through yet (#1270).
        if (dev.handle == nullptr || Impl::asleep(dev))
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
        if (pciChanged || m_Impl->gpuLost || m_Impl->loadRetryPending)
        {
            const char* reason = "retrying a failed start";
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
            // Report a change only if the restart worked, or if no NVIDIA-driver GPU is left (an empty
            // list is then the truth). A transient failure with the GPUs still present reports no
            // change, so GPUModel keeps the known GPU list -- their readings are gaps meanwhile -- and
            // the next full rescan retries, instead of publishing an empty list.
            return isAvailable() || !PciDisplayDevices::anyBoundTo(m_Impl->pciDevicesSeen, PciDisplayDevices::DRIVER_NVIDIA);
        }
        // A deferred lookup that failed transiently (NVML_ERROR_UNKNOWN, say) without losing the GPU
        // triggers no restart, so it is retried here, at the full-rescan cadence: the check below then
        // asks for the re-enumeration that looks it up again.
        for (auto& device : m_Impl->devices)
        {
            device.resolveFailed = false;
        }
    }

    // An adapter asleep when it was enumerated and awake now: enumerate again to look it up if it was
    // deferred (#1270) and to find its sensors (#1289). Reads only runtime_status, and only for such
    // adapters.
    return isAvailable() &&
           std::ranges::any_of(m_Impl->devices,
                               [](const Impl::Device& device) { return Impl::awaitingWake(device) && !Impl::asleep(device); });
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
