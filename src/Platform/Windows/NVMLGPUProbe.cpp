#include "NVMLGPUProbe.h"

#include "DisplayDevicePower.h"
#include "Platform/GPUTypes.h"
#include "Platform/NVMLRunningProcesses.h"
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
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

// Import NVML types from shared header
using namespace Platform::NVML;

namespace Platform
{

NVMLGPUProbe::NVMLGPUProbe()
    : m_Initialized(loadNVML() && initializeNVML()),
      m_DevicePower(std::make_shared<DisplayDevicePower>()),
      m_IsAsleep([power = m_DevicePower](const PciLocation& location) { return power->isAsleep(location); })
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
    // Defensive guard: today loadNVML() is only ever called once (from the constructor), so
    // m_NVMLHandle is always null here, but a future retry-on-failure/hot-reload path calling
    // this twice would otherwise overwrite the handle without a matching FreeLibrary, leaking
    // one DLL reference count (#781).
    if (m_NVMLHandle != nullptr)
    {
        return true;
    }

    // NVIDIA installs NVML in System32. Restrict the search to that trusted
    // directory so a portable installation cannot load an adjacent DLL.
    m_NVMLHandle = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m_NVMLHandle == nullptr)
    {
        spdlog::debug("NVMLGPUProbe: Failed to load nvml.dll (NVIDIA driver not installed)");
        return false;
    }

    // Load function pointers. On a missing symbol, unloadNVML() first (rather than just
    // `return false`): otherwise m_NVMLHandle stays non-null after a partial load, and the
    // "already loaded" guard above would then report success on a later retry despite this
    // function's pointers never having been fully resolved.
#define LOAD_NVML_FUNC(name)                                                                                                               \
    m_NVML.name = reinterpret_cast<decltype(m_NVML.name)>(GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvml" #name));               \
    if (m_NVML.name == nullptr)                                                                                                            \
    {                                                                                                                                      \
        spdlog::warn("NVMLGPUProbe: Failed to load nvml" #name);                                                                           \
        unloadNVML();                                                                                                                      \
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
        unloadNVML();
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
    // The running-process entry points come in three variants writing two entry layouts; take the
    // newest exported and remember its entry size (#1313, as Linux does since #1092).
    const auto resolve = [module = static_cast<HMODULE>(m_NVMLHandle)](const std::string& name)
    {
        // GetProcAddress returns FARPROC; the chooser deals in plain addresses, cast back at load.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<void*>(GetProcAddress(module, name.c_str()));
    };
    m_NVML.DeviceGetComputeRunningProcesses = loadRunningProcessesQuery("nvmlDeviceGetComputeRunningProcesses", resolve);
    m_NVML.DeviceGetGraphicsRunningProcesses = loadRunningProcessesQuery("nvmlDeviceGetGraphicsRunningProcesses", resolve);
    // nvml.h maps nvmlDeviceGetPciInfo to the _v3 export; older drivers have only _v2 (same struct).
    m_NVML.DeviceGetPciInfo =
        reinterpret_cast<decltype(m_NVML.DeviceGetPciInfo)>(GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvmlDeviceGetPciInfo_v3"));
    if (m_NVML.DeviceGetPciInfo == nullptr)
    {
        m_NVML.DeviceGetPciInfo = reinterpret_cast<decltype(m_NVML.DeviceGetPciInfo)>(
            GetProcAddress(static_cast<HMODULE>(m_NVMLHandle), "nvmlDeviceGetPciInfo_v2"));
    }

#undef LOAD_NVML_FUNC_OPTIONAL
#undef LOAD_NVML_FUNC

    spdlog::debug("NVMLGPUProbe: Successfully loaded nvml.dll");
    return true;
}

NVMLGPUProbe::RunningProcessesQuery NVMLGPUProbe::loadRunningProcessesQuery(std::string_view baseName,
                                                                            const std::function<void*(const std::string&)>& resolve)
{
    const auto symbol = NVMLRunningProcesses::chooseRunningProcessesSymbol(baseName, resolve);
    if (symbol.address == nullptr)
    {
        spdlog::debug("NVMLGPUProbe: {} not available (optional)", baseName);
        return {};
    }
    spdlog::debug("NVMLGPUProbe: using {} ({}-byte entries)", symbol.name, symbol.entrySize);
    // The address came from GetProcAddress (or a test's fake export table); the cast to the known
    // NVML signature is required.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {.fn = reinterpret_cast<RunningProcessesFn>(symbol.address), .entrySize = symbol.entrySize};
}

void NVMLGPUProbe::unloadNVML()
{
    if (m_NVMLHandle != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(m_NVMLHandle));
        m_NVMLHandle = nullptr;
    }
    // The pointers lead into the freed module: clear them, so isLoaded() is false and nothing calls
    // through one before a reload.
    m_NVML = NVMLFunctions{};
}

// NOLINTNEXTLINE(readability-make-member-function-const) - Calls NVML init which has global side effects
bool NVMLGPUProbe::initializeNVML(bool quietFailure)
{
    if (m_NVML.Init == nullptr)
    {
        return false;
    }

    nvmlReturn_t result = m_NVML.Init();
    if (result != NVML_SUCCESS)
    {
        spdlog::log(
            quietFailure ? spdlog::level::debug : spdlog::level::warn, "NVMLGPUProbe: nvmlInit failed: {}", getNVMLErrorString(result));
        return false;
    }

    // Get driver version
    std::array<char, NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE> driverVersion{};
    result = m_NVML.SystemGetDriverVersion == nullptr
               ? NVML_ERROR_FUNCTION_NOT_FOUND
               : m_NVML.SystemGetDriverVersion(driverVersion.data(), NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE);
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

bool NVMLGPUProbe::isDeviceAsleep(uint32_t index) const
{
    const auto location = m_DevicePciLocations.find(index);
    return location != m_DevicePciLocations.end() && m_IsAsleep && m_IsAsleep(location->second);
}

bool NVMLGPUProbe::isResetResult(nvmlReturn_t result)
{
    return result == NVML_ERROR_GPU_IS_LOST || result == NVML_ERROR_UNINITIALIZED;
}

nvmlReturn_t NVMLGPUProbe::noteResult(nvmlReturn_t result)
{
    if (isResetResult(result))
    {
        if (!m_GPULost)
        {
            spdlog::warn("NVMLGPUProbe: NVML reported {}; re-initialising at the next full rescan", getNVMLErrorString(result));
        }
        m_GPULost = true;
    }
    return result;
}

bool NVMLGPUProbe::restart()
{
    // What each device learnt while it was known, by id: one asleep now can't be asked again (#1265).
    for (const auto& [index, id] : m_DeviceIds)
    {
        Remembered remembered;
        if (const auto total = m_LastMemoryTotals.find(index); total != m_LastMemoryTotals.end())
        {
            remembered.lastMemoryTotalBytes = total->second;
            remembered.hasMemoryTotal = true;
        }
        if (const auto details = m_DeviceDetails.find(index); details != m_DeviceDetails.end())
        {
            remembered.driverVersion = details->second.driverVersion;
            remembered.sensors = details->second.sensors;
        }
        m_Remembered.insert_or_assign(id, std::move(remembered));
    }

    // nvmlShutdown() then nvmlInit() is NVML's supported way to start over; the device handles of
    // the old session are dropped with it.
    ++m_RestartCount;
    shutdownNVML();
    m_GPULost = false;
    const bool quiet = m_RestartFailures > 0;
    if (!isLoaded() && !loadNVML())
    {
        ++m_RestartFailures;
        spdlog::log(quiet ? spdlog::level::debug : spdlog::level::info,
                    "NVMLGPUProbe: NVML re-initialisation failed (nvml.dll not loaded)");
        return false;
    }
    m_Initialized = initializeNVML(quiet);
    if (!m_Initialized)
    {
        ++m_RestartFailures;
        return false;
    }
    m_RestartFailures = 0;
    spdlog::info("NVMLGPUProbe: NVML re-initialised");
    return true;
}

bool NVMLGPUProbe::rescanGPUs(GPURescan depth)
{
    // A lost GPU waits for a full rescan rather than re-initialising NVML on the next sample, so one
    // that stays lost costs a re-init per interval, not per sample.
    if (depth == GPURescan::Full && m_GPULost)
    {
        return restart();
    }
    // While lost, a device whose sensors are unknown stays unknown until the full rescan restarts
    // NVML: re-enumerating at every quick rescan would only probe the lost GPU again.
    if (!m_Initialized || m_GPULost)
    {
        return false;
    }
    // A GPU asleep when it was enumerated and awake now: enumerate again to find its sensors
    // (#1294). Asks only the PnP power state, and only for such GPUs.
    return std::ranges::any_of(m_DeviceIds,
                               [this](const auto& entry)
                               {
                                   const auto details = m_DeviceDetails.find(entry.first);
                                   const bool sensorsKnown = details != m_DeviceDetails.end() && details->second.sensors.has_value();
                                   return !sensorsKnown && !isDeviceAsleep(entry.first);
                               });
}

void NVMLGPUProbe::shutdownNVML()
{
    m_DeviceHandles.clear();
    m_DeviceIds.clear();
    m_DevicePciLocations.clear();
    m_LastMemoryTotals.clear();
    m_DeviceDetails.clear();

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
    const nvmlReturn_t result = noteResult(m_NVML.DeviceGetCount(&deviceCount));
    if (result != NVML_SUCCESS)
    {
        spdlog::warn("NVMLGPUProbe: DeviceGetCount failed: {}", getNVMLErrorString(result));
        return gpus;
    }

    // Adapters may have come or gone since the last enumeration: look their devnodes up afresh.
    if (m_DevicePower)
    {
        m_DevicePower->reset();
    }

    // Enumerate devices
    for (unsigned int i = 0; i < deviceCount; ++i)
    {
        // A device's identity is read once per NVML session, so a repeat enumeration (GPUModel
        // re-enumerates whenever rescanGPUs() reports a change) makes no call to a known device,
        // asleep or not (#1294).
        if (!m_DeviceIds.contains(i) && !readDeviceIdentity(i))
        {
            continue;
        }
        auto& details = m_DeviceDetails[i];

        GPUInfo info{};
        info.id = m_DeviceIds[i];
        info.name = details.name;
        // NVML only works with NVIDIA GPUs
        info.vendor = "NVIDIA";
        // NVIDIA discrete GPUs (NVML doesn't expose integrated GPUs typically)
        info.isIntegrated = false;
        info.deviceIndex = i;
        // PCI identity, so the Windows probe can match this device to its DXGI adapter by hardware
        // rather than by name or enumeration order (#1091), and so a sleeping GPU can be left alone
        // (#1265).
        if (const auto location = m_DevicePciLocations.find(i); location != m_DevicePciLocations.end())
        {
            info.pciLocation = location->second;
        }
        info.pciDeviceId = details.pciDeviceId;

        // Which sensors this device actually reports: capabilities() covers NVML as a whole, but
        // e.g. a passively cooled card has no fan reading, and a laptop GPU may not report power
        // (#1040). A read that fails now is treated as unsupported for this device. Found once, the
        // first time the device is seen awake: a sleeping GPU gets no VBIOS read or sensor probe,
        // which could wake it (#1265), so until then its sensor set is the probe's
        // (sensorCapabilities unset), as on Linux (#1117), and rescanGPUs() asks for a
        // re-enumeration once it wakes (#1294). A probe that finds the GPU lost or NVML
        // uninitialised (a driver reset mid-probe) proves nothing about the sensors: it records the
        // loss, so the next full rescan restarts NVML, and leaves the set unknown, to be found once
        // NVML is back rather than cached (and kept across the restart) as "no sensors".
        if (!details.sensors.has_value())
        {
            if (isDeviceAsleep(i))
            {
                spdlog::debug("NVMLGPUProbe: NVIDIA GPU {} ({}) is asleep; not probing its sensors", i, info.name);
            }
            else
            {
                nvmlDevice_t device = m_DeviceHandles[i];
                bool reset = false;
                // Whether a probe read succeeded, noting a lost GPU or uninitialised NVML.
                const auto succeeded = [this, &reset](nvmlReturn_t result)
                {
                    reset = reset || isResetResult(noteResult(result));
                    return result == NVML_SUCCESS;
                };
                std::array<char, NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE> vbiosVersion{};
                if (succeeded(m_NVML.DeviceGetVbiosVersion(device, vbiosVersion.data(), NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE)))
                {
                    details.driverVersion = vbiosVersion.data();
                }

                unsigned int probeValue = 0;
                GPUCapabilities sensors = capabilities();
                sensors.hasTemperature = succeeded(m_NVML.DeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &probeValue));
                sensors.hasPowerMetrics = succeeded(m_NVML.DeviceGetPowerUsage(device, &probeValue));
                sensors.hasClockSpeeds = succeeded(m_NVML.DeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &probeValue));
                sensors.hasFanSpeed = succeeded(m_NVML.DeviceGetFanSpeed(device, &probeValue));
                if (reset)
                {
                    spdlog::debug("NVMLGPUProbe: NVIDIA GPU {} ({}) was lost while probing its sensors; probing again after NVML restarts",
                                  i,
                                  info.name);
                }
                else
                {
                    details.sensors = sensors;
                }
            }
        }
        info.driverVersion = details.driverVersion;
        info.sensorCapabilities = details.sensors;

        spdlog::debug("NVMLGPUProbe: Enumerated NVIDIA GPU {}: {}", i, info.name);

        gpus.push_back(std::move(info));
    }

    spdlog::info("NVMLGPUProbe: Enumerated {} NVIDIA GPU(s)", gpus.size());
    return gpus;
}

bool NVMLGPUProbe::readDeviceIdentity(uint32_t index)
{
    nvmlDevice_t device = nullptr;
    nvmlReturn_t result = noteResult(m_NVML.DeviceGetHandleByIndex(index, &device));
    if (result != NVML_SUCCESS || device == nullptr)
    {
        spdlog::warn("NVMLGPUProbe: DeviceGetHandleByIndex({}) failed: {}", index, getNVMLErrorString(result));
        return false;
    }

    // Store device handle for later use
    m_DeviceHandles[index] = device;
    DeviceDetails details;

    // Get device name
    std::array<char, NVML_DEVICE_NAME_BUFFER_SIZE> name{};
    if (m_NVML.DeviceGetName(device, name.data(), NVML_DEVICE_NAME_BUFFER_SIZE) == NVML_SUCCESS)
    {
        details.name = name.data();
    }

    // Get device UUID (unique identifier); fall back to an index-based id
    std::array<char, NVML_DEVICE_UUID_BUFFER_SIZE> uuid{};
    result = m_NVML.DeviceGetUUID(device, uuid.data(), NVML_DEVICE_UUID_BUFFER_SIZE);
    std::string id = result == NVML_SUCCESS ? std::string(uuid.data()) : std::format("NVML_GPU{}", index);

    // PCI identity, read once: it matches the device to its DXGI adapter (#1091) and says where to
    // ask whether it is asleep (#1265).
    m_DevicePciLocations.erase(index);
    if (m_NVML.DeviceGetPciInfo != nullptr)
    {
        NVML::nvmlPciInfo_t pci{};
        if (m_NVML.DeviceGetPciInfo(device, &pci) == NVML_SUCCESS)
        {
            m_DevicePciLocations[index] = PciLocation{.bus = pci.bus, .device = pci.device, .function = NVML::pciFunction(pci)};
            details.pciDeviceId = pci.pciDeviceId;
        }
    }

    // A device known before an NVML restart keeps what was found for it then (#1294).
    if (const auto remembered = m_Remembered.find(id); remembered != m_Remembered.end())
    {
        details.driverVersion = remembered->second.driverVersion;
        details.sensors = remembered->second.sensors;
        if (remembered->second.hasMemoryTotal)
        {
            m_LastMemoryTotals[index] = remembered->second.lastMemoryTotalBytes;
        }
        m_Remembered.erase(remembered);
    }
    m_DeviceIds[index] = std::move(id);
    m_DeviceDetails[index] = std::move(details);
    return true;
}

std::string NVMLGPUProbe::deviceId(std::uint32_t index, nvmlDevice_t device) const
{
    if (const auto knownId = m_DeviceIds.find(index); knownId != m_DeviceIds.end())
    {
        return knownId->second;
    }
    std::array<char, NVML_DEVICE_UUID_BUFFER_SIZE> uuid{};
    const nvmlReturn_t result = m_NVML.DeviceGetUUID != nullptr ? m_NVML.DeviceGetUUID(device, uuid.data(), NVML_DEVICE_UUID_BUFFER_SIZE)
                                                                : NVML_ERROR_NOT_SUPPORTED;
    return result == NVML_SUCCESS ? std::string(uuid.data()) : std::format("NVML_GPU{}", index);
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

        // The id enumeration reported for this device, so the two always agree.
        counter.gpuId = deviceId(index, device);

        // A sleeping GPU gets no NVML query at all, which could wake it (#1265): every reading is
        // unavailable this sample, and the VRAM total is the last one read while it was awake.
        if (isDeviceAsleep(index))
        {
            counter.suspended = true;
            counter.utilizationAvailable = false;
            counter.temperatureAvailable = false;
            counter.powerAvailable = false;
            counter.gpuClockAvailable = false;
            counter.memoryAvailable = false;
            if (const auto total = m_LastMemoryTotals.find(index); total != m_LastMemoryTotals.end())
            {
                counter.memoryTotalBytes = total->second;
            }
            counters.push_back(std::move(counter));
            continue;
        }

        // Memory info (raw counters only)
        nvmlMemory_t memInfo{};
        nvmlReturn_t result = noteResult(m_NVML.DeviceGetMemoryInfo(device, &memInfo));
        if (result == NVML_SUCCESS)
        {
            counter.memoryUsedBytes = memInfo.used;
            counter.memoryTotalBytes = memInfo.total;
            m_LastMemoryTotals[index] = memInfo.total;
        }
        else
        {
            counter.memoryAvailable = false; // Unread this sample: not a real 0% (#1111)
        }

        // Temperature (GPU die)
        unsigned int temp = 0;
        result = noteResult(m_NVML.DeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp));
        if (result == NVML_SUCCESS)
        {
            counter.temperatureC = static_cast<int32_t>(temp);
        }
        else
        {
            counter.temperatureAvailable = false; // Unread this sample (timeout, GPU lost, TDR): not a real 0 (#1111)
        }

        // Power usage (milliwatts) - raw counter only
        unsigned int powerMilliwatts = 0;
        result = noteResult(m_NVML.DeviceGetPowerUsage(device, &powerMilliwatts));
        if (result == NVML_SUCCESS)
        {
            counter.powerDrawWatts = static_cast<double>(powerMilliwatts) / 1000.0;
        }
        else
        {
            counter.powerAvailable = false; // Unread this sample (timeout, GPU lost, TDR): not a real 0 (#1111)
        }

        // Power limit (milliwatts) - raw counter only
        unsigned int powerLimitMilliwatts = 0;
        result = noteResult(m_NVML.DeviceGetPowerManagementLimit(device, &powerLimitMilliwatts));
        if (result == NVML_SUCCESS)
        {
            counter.powerLimitWatts = static_cast<double>(powerLimitMilliwatts) / 1000.0;
        }

        // GPU clock (MHz)
        unsigned int gpuClock = 0;
        result = noteResult(m_NVML.DeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &gpuClock));
        if (result == NVML_SUCCESS)
        {
            counter.gpuClockMHz = gpuClock;
        }
        else
        {
            counter.gpuClockAvailable = false; // Unread this sample (timeout, GPU lost, TDR): not a real 0 (#1111)
        }

        // Memory clock (MHz)
        unsigned int memClock = 0;
        result = noteResult(m_NVML.DeviceGetClockInfo(device, NVML_CLOCK_MEM, &memClock));
        if (result == NVML_SUCCESS)
        {
            counter.memoryClockMHz = memClock;
        }

        // GPU utilization
        nvmlUtilization_t util{};
        result = noteResult(m_NVML.DeviceGetUtilizationRates(device, &util));
        if (result == NVML_SUCCESS)
        {
            counter.utilizationPercent = static_cast<double>(util.gpu);
        }
        else
        {
            counter.utilizationAvailable = false; // Unread this sample (timeout, GPU lost, TDR): not a real 0 (#1111)
        }

        // Fan speed: NVML returns percentage 0-100 directly, so the max is always 100 (see
        // GPUCounters::fanSpeedRaw/fanSpeedMaxRaw; Domain computes the percent).
        unsigned int fanSpeed = 0;
        result = noteResult(m_NVML.DeviceGetFanSpeed(device, &fanSpeed));
        if (result == NVML_SUCCESS)
        {
            counter.fanSpeedRaw = fanSpeed;
            counter.fanSpeedMaxRaw = 100;
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
    const auto& computeQuery = m_NVML.DeviceGetComputeRunningProcesses;
    const auto& graphicsQuery = m_NVML.DeviceGetGraphicsRunningProcesses;
    if (computeQuery.fn == nullptr && graphicsQuery.fn == nullptr)
    {
        spdlog::debug("NVMLGPUProbe: Per-process GPU functions not available");
        return allCounters;
    }

    // One list from one entry point, parsed by the entry size of the variant loaded (#1313). The
    // shared query caps the count at MAX_PLAUSIBLE_PROCESS_COUNT, so a buggy/corrupted driver
    // reporting an implausible count cannot force a huge allocation; that is logged here.
    const auto runningProcesses = [this](const RunningProcessesQuery& query, nvmlDevice_t device, uint32_t index, std::string_view what)
    {
        if (query.fn == nullptr)
        {
            return std::vector<NVMLRunningProcesses::RunningProcess>{};
        }
        nvmlReturn_t lastResult = NVML_SUCCESS;
        auto processes = NVMLRunningProcesses::queryRunningProcesses(
            [&](unsigned int* count, void* buffer)
            {
                lastResult = query.fn(device, count, static_cast<nvmlProcessInfoEntries*>(buffer));
                if (buffer == nullptr && *count > NVMLRunningProcesses::MAX_PLAUSIBLE_PROCESS_COUNT)
                {
                    spdlog::warn("NVMLGPUProbe: {} reported implausible count {} on GPU {}, skipping", what, *count, index);
                }
                return lastResult;
            },
            query.entrySize);
        // A lost GPU or uninitialised NVML here restarts NVML at the next full rescan, as a counter
        // read's does (#1294).
        static_cast<void>(noteResult(lastResult));
        if (lastResult != NVML_SUCCESS && lastResult != NVML_ERROR_INSUFFICIENT_SIZE && lastResult != NVML_ERROR_NOT_SUPPORTED)
        {
            spdlog::debug("NVMLGPUProbe: {} returned {}", what, static_cast<unsigned int>(lastResult));
        }
        else if (!processes.empty())
        {
            spdlog::debug("NVMLGPUProbe: Found {} {} entries on GPU {}", processes.size(), what, index);
        }
        return processes;
    };

    for (const auto& [index, device] : m_DeviceHandles)
    {
        if (isDeviceAsleep(index))
        {
            continue; // Not queried while asleep, which could wake it (#1265)
        }

        // The id of the DXGI adapter this device is (see setProcessGpuIds()), or the device's own,
        // recorded with its handle at enumeration. Not "GPU{index}": NVML's numbering is not DXGI's,
        // so that named another adapter (#1317). Nothing is asked of the device to name it here.
        const auto knownId = m_DeviceIds.find(index);
        std::string gpuId = knownId != m_DeviceIds.end() ? knownId->second : std::format("NVML_GPU{}", index);
        if (const auto adapterId = m_ProcessGpuIds.find(gpuId); adapterId != m_ProcessGpuIds.end())
        {
            gpuId = adapterId->second;
        }

        // Compute processes (CUDA, OpenCL) and graphics processes (DirectX, OpenGL, Vulkan), one
        // row per process: the larger figure where both lists report one allocation, MIG instances
        // summed. Memory NVML can't report counts as 0.
        const auto compute = runningProcesses(computeQuery, device, index, "DeviceGetComputeRunningProcesses");
        const auto graphics = runningProcesses(graphicsQuery, device, index, "DeviceGetGraphicsRunningProcesses");
        for (const auto& usage : NVMLRunningProcesses::combineRunningProcesses(compute, graphics))
        {
            ProcessGPUCounters counter;
            counter.pid = static_cast<std::int32_t>(usage.pid);
            counter.gpuId = gpuId;
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
    // NVML only returns PCIe throughput as rates, not cumulative counters, so
    // pcieTxBytes/pcieRxBytes are deliberately left at 0 (see the comment where counters
    // are populated above) -- report the capability as unavailable, not present-but-zero.
    caps.hasPCIeMetrics = false;
    caps.hasEngineUtilization = true;
    // Per-process metrics available if we have the required functions
    caps.hasPerProcessMetrics =
        (m_NVML.DeviceGetComputeRunningProcesses.fn != nullptr || m_NVML.DeviceGetGraphicsRunningProcesses.fn != nullptr);
    caps.hasEncoderDecoder = false; // Not implemented yet
    caps.supportsMultiGPU = true;

    return caps;
}

} // namespace Platform
