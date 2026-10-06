#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "Platform/NVMLTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform
{

class DisplayDevicePower;

/// NVIDIA GPU probe using NVML (NVIDIA Management Library).
/// Provides enhanced metrics for NVIDIA GPUs: temperature, power, clock speeds, etc.
/// Requires NVIDIA driver 450+ and NVML 11+.
/// Uses dynamic loading with graceful fallback if NVML is unavailable.
///
/// A GPU that is asleep (a hybrid laptop's runtime-suspended dGPU; see DisplayDevicePower) is not
/// queried, since NVML calls can wake it and keep it awake (#1265): its counters are marked
/// GPUCounters::suspended with every reading unavailable, and enumeration skips its sensor probe.
///
/// Each device's identity (handle, name, UUID, PCI) is read once per NVML session and its sensor set
/// once it is first seen awake, so a repeat enumerateGPUs() makes no call to a known device (#1294).
/// rescanGPUs() re-initialises NVML at a full rescan after a query reported the GPU lost or NVML
/// uninitialised (a driver reset, say), and on a quick one asks for a re-enumeration once a GPU that
/// was asleep at enumeration wakes, so its own sensor set is found (#1294, as Linux does #1289).
class NVMLGPUProbe : public IGPUProbe
{
  public:
    NVMLGPUProbe();
    ~NVMLGPUProbe() override;

    // Rule of 5
    NVMLGPUProbe(const NVMLGPUProbe&) = delete;
    NVMLGPUProbe& operator=(const NVMLGPUProbe&) = delete;
    NVMLGPUProbe(NVMLGPUProbe&&) = delete;
    NVMLGPUProbe& operator=(NVMLGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
    /// Full: re-initialise NVML if a query since the last (re)start reported NVML_ERROR_GPU_IS_LOST or
    /// NVML_ERROR_UNINITIALIZED, reporting a change if that worked. Either depth: report a change
    /// when a GPU that was asleep at enumeration (so its sensors are unknown) is awake now. Asking
    /// whether it is awake doesn't wake it (#1265, #1294).
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

    /// Shut NVML down and start it again (loading nvml.dll first if it isn't loaded), so the next
    /// enumerateGPUs() lists the NVIDIA GPUs present now: after a GPU was lost, or when the adapter
    /// set changed (#1294). A device that comes back keeps the VRAM total and sensor set already
    /// found for it, since one asleep now isn't woken to find them again. Returns isAvailable().
    bool restart();

    /// The id each device's per-process counters carry, by the device's own id (its UUID): the id of
    /// the DXGI adapter WindowsGPUProbe matched it to by PCI location, so a process is attributed to
    /// the same GPU the GPU tab shows. NVML numbers its devices in its own order, not DXGI's, so the
    /// "GPU{nvmlIndex}" these used to carry could name another adapter (#1091, #1317). A device with
    /// no entry keeps its own id. Replaces the previous set.
    void setProcessGpuIds(std::unordered_map<std::string, std::string> adapterIdByDeviceId)
    {
        m_ProcessGpuIds = std::move(adapterIdByDeviceId);
    }

    /// Check if NVML is available and initialized
    [[nodiscard]] bool isAvailable() const
    {
        return m_Initialized;
    }

    /// Whether nvml.dll is loaded, i.e. the NVIDIA driver is installed, whether or not NVML started.
    [[nodiscard]] bool isLoaded() const
    {
        return m_NVML.Init != nullptr;
    }

  private:
    // Test-only accessor: lets unit tests substitute fake NVML function pointers and device
    // handles after construction, so enumerateGPUs()/readGPUCounters()/readProcessGPUCounters()/
    // capabilities() can be exercised deterministically without a real NVIDIA GPU or nvml.dll.
    // The constructor's loadNVML()/initializeNVML() still run as normal before the accessor
    // substitutes the backend; this does not change or bypass loadNVML()'s
    // LOAD_LIBRARY_SEARCH_SYSTEM32 hardening in any way. Production code never touches this -
    // only test_WindowsNVMLGPUProbe.cpp uses it.
    friend struct NVMLGPUProbeTestAccessor;

    bool loadNVML();
    void unloadNVML();
    /// nvmlInit(). @p quietFailure logs a failure at debug rather than warn (a repeated retry).
    bool initializeNVML(bool quietFailure = false);
    void shutdownNVML();

    /// Note a device query's result: a lost GPU or an uninitialised library means NVML must be
    /// re-initialised (#1294). Returns @p result unchanged.
    NVML::nvmlReturn_t noteResult(NVML::nvmlReturn_t result);

    [[nodiscard]] static std::string getNVMLErrorString(NVML::nvmlReturn_t result);

    /// The id enumeration reported for the device at @p index, so every reading agrees with it; the
    /// UUID is queried only for a device enumeration did not record ("NVML_GPU{index}" without one).
    [[nodiscard]] std::string deviceId(std::uint32_t index, NVML::nvmlDevice_t device) const;

    // A running-process entry point and the size of the entries it writes (#1313): the legacy
    // unversioned export writes 16-byte nvmlProcessInfo_v1_t entries, _v2/_v3 write 24-byte
    // nvmlProcessInfo_v2_t ones, so the struct is opaque here and the entries are parsed by size
    // (NVMLRunningProcesses). The third parameter is the shared NVML::nvmlProcessInfoEntries, which
    // the test fake's definitions also take, so the call matches the callee's own function type.
    using RunningProcessesFn = NVML::nvmlReturn_t (*)(NVML::nvmlDevice_t, unsigned int*, NVML::nvmlProcessInfoEntries*);
    struct RunningProcessesQuery
    {
        RunningProcessesFn fn = nullptr;
        std::size_t entrySize = 0;
    };

    /// The newest of `baseName`'s _v3, _v2 and unversioned exports that `resolve(name)` finds
    /// (nullptr when it isn't exported), with its entry size; an empty query if none is (#1313).
    [[nodiscard]] static RunningProcessesQuery loadRunningProcessesQuery(std::string_view baseName,
                                                                         const std::function<void*(const std::string&)>& resolve);

    // NVML function pointers (dynamically loaded)
    struct NVMLFunctions
    {
        NVML::nvmlReturn_t (*Init)() = nullptr;
        NVML::nvmlReturn_t (*Shutdown)() = nullptr;
        NVML::nvmlReturn_t (*DeviceGetCount)(unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, NVML::nvmlDevice_t*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetName)(NVML::nvmlDevice_t, char*, unsigned int) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetUUID)(NVML::nvmlDevice_t, char*, unsigned int) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetMemoryInfo)(NVML::nvmlDevice_t, void*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetTemperature)(NVML::nvmlDevice_t, int, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetPowerUsage)(NVML::nvmlDevice_t, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetPowerManagementLimit)(NVML::nvmlDevice_t, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetClockInfo)(NVML::nvmlDevice_t, int, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetMaxClockInfo)(NVML::nvmlDevice_t, int, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetUtilizationRates)(NVML::nvmlDevice_t, void*) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetPcieThroughput)(NVML::nvmlDevice_t, int, unsigned int*) = nullptr;
        NVML::nvmlReturn_t (*SystemGetDriverVersion)(char*, unsigned int) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetVbiosVersion)(NVML::nvmlDevice_t, char*, unsigned int) = nullptr;
        NVML::nvmlReturn_t (*DeviceGetFanSpeed)(NVML::nvmlDevice_t, unsigned int*) = nullptr;
        // PCI identity, to match NVML devices to DXGI adapters (#1091); optional
        NVML::nvmlReturn_t (*DeviceGetPciInfo)(NVML::nvmlDevice_t, NVML::nvmlPciInfo_t*) = nullptr;
        // Per-process GPU functions, each the newest variant nvml.dll exports (#1313)
        RunningProcessesQuery DeviceGetComputeRunningProcesses;
        RunningProcessesQuery DeviceGetGraphicsRunningProcesses;
    };

    void* m_NVMLHandle{nullptr};
    NVMLFunctions m_NVML{};
    bool m_Initialized{false};

    // Map device index to NVML handle
    std::unordered_map<uint32_t, NVML::nvmlDevice_t> m_DeviceHandles;
    // Map device index to the id enumerateGPUs() reported (UUID, or NVML_GPU<n> when that read
    // failed). Counter reads reuse it rather than querying the UUID again: a second, independently
    // fallible query could give a device a different id and lose its NVML metrics (#1040).
    std::unordered_map<uint32_t, std::string> m_DeviceIds;
    // setProcessGpuIds(): per-process counters' gpuId by device id (#1317).
    std::unordered_map<std::string, std::string> m_ProcessGpuIds;

    /// Read device @p index's handle, name, UUID and PCI identity into the per-device maps, restoring
    /// what a restart() remembered for its id. False if NVML gives no handle for it (#1294).
    bool readDeviceIdentity(uint32_t index);

    /// Whether device @p index is asleep now, so must not be queried (#1265). False for a device
    /// whose PCI location NVML didn't report.
    [[nodiscard]] bool isDeviceAsleep(uint32_t index) const;

    // Map device index to its PCI location, from enumeration, to ask whether it is asleep (#1265).
    std::unordered_map<uint32_t, PciLocation> m_DevicePciLocations;
    // Map device index to the VRAM total last read while it was awake, still reported while it
    // sleeps so the adapter's size doesn't vanish (#1265).
    std::unordered_map<uint32_t, std::uint64_t> m_LastMemoryTotals;

    // What enumeration learnt about each device beyond its id, by device index (#1294).
    struct DeviceDetails
    {
        std::string name;
        std::uint32_t pciDeviceId = 0;
        std::string driverVersion; // VBIOS version, read with the sensors
        // Which sensors it reports, found the first time enumeration sees it awake (#1040); unset
        // while it has only been seen asleep, since it isn't woken to find out (#1265).
        std::optional<GPUCapabilities> sensors;
    };
    std::unordered_map<uint32_t, DeviceDetails> m_DeviceDetails;

    // What a device had learnt, by id, kept across restart() for when it comes back (#1294).
    struct Remembered
    {
        std::uint64_t lastMemoryTotalBytes = 0;
        bool hasMemoryTotal = false;
        std::string driverVersion;
        std::optional<GPUCapabilities> sensors;
    };
    std::unordered_map<std::string, Remembered> m_Remembered;

    // A device query reported NVML_ERROR_GPU_IS_LOST or NVML_ERROR_UNINITIALIZED since the last
    // (re)start, so the next full rescan re-initialises NVML (#1294).
    bool m_GPULost{false};
    // Consecutive restarts that failed; after the first, a failure is logged quietly.
    int m_RestartFailures{0};

    // The non-waking PnP power query; its devnode cache is dropped at each enumeration (#1265).
    std::shared_ptr<DisplayDevicePower> m_DevicePower;
    // Whether the GPU at a PCI location is asleep; m_DevicePower's answer in production, a fake in
    // tests (#1265).
    std::function<bool(const PciLocation&)> m_IsAsleep;
};

} // namespace Platform
