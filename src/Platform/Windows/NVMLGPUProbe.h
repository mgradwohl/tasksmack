#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"
#include "Platform/NVMLTypes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Platform
{

/// NVIDIA GPU probe using NVML (NVIDIA Management Library).
/// Provides enhanced metrics for NVIDIA GPUs: temperature, power, clock speeds, etc.
/// Requires NVIDIA driver 450+ and NVML 11+.
/// Uses dynamic loading with graceful fallback if NVML is unavailable.
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

    /// Check if NVML is available and initialized
    [[nodiscard]] bool isAvailable() const
    {
        return m_Initialized;
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
    bool initializeNVML();
    void shutdownNVML();

    [[nodiscard]] static std::string getNVMLErrorString(NVML::nvmlReturn_t result);

    // NVML function pointers (dynamically loaded)
    struct NVMLFunctions
    {
        NVML::nvmlReturn_t (*Init)();
        NVML::nvmlReturn_t (*Shutdown)();
        NVML::nvmlReturn_t (*DeviceGetCount)(unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, NVML::nvmlDevice_t*);
        NVML::nvmlReturn_t (*DeviceGetName)(NVML::nvmlDevice_t, char*, unsigned int);
        NVML::nvmlReturn_t (*DeviceGetUUID)(NVML::nvmlDevice_t, char*, unsigned int);
        NVML::nvmlReturn_t (*DeviceGetMemoryInfo)(NVML::nvmlDevice_t, void*);
        NVML::nvmlReturn_t (*DeviceGetTemperature)(NVML::nvmlDevice_t, int, unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetPowerUsage)(NVML::nvmlDevice_t, unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetPowerManagementLimit)(NVML::nvmlDevice_t, unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetClockInfo)(NVML::nvmlDevice_t, int, unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetMaxClockInfo)(NVML::nvmlDevice_t, int, unsigned int*);
        NVML::nvmlReturn_t (*DeviceGetUtilizationRates)(NVML::nvmlDevice_t, void*);
        NVML::nvmlReturn_t (*DeviceGetPcieThroughput)(NVML::nvmlDevice_t, int, unsigned int*);
        NVML::nvmlReturn_t (*SystemGetDriverVersion)(char*, unsigned int);
        NVML::nvmlReturn_t (*DeviceGetVbiosVersion)(NVML::nvmlDevice_t, char*, unsigned int);
        NVML::nvmlReturn_t (*DeviceGetFanSpeed)(NVML::nvmlDevice_t, unsigned int*);
        // Per-process GPU functions
        NVML::nvmlReturn_t (*DeviceGetComputeRunningProcesses)(NVML::nvmlDevice_t, unsigned int*, void*);
        NVML::nvmlReturn_t (*DeviceGetGraphicsRunningProcesses)(NVML::nvmlDevice_t, unsigned int*, void*);
    };

    void* m_NVMLHandle{nullptr};
    NVMLFunctions m_NVML{};
    bool m_Initialized{false};

    // Map device index to NVML handle
    std::unordered_map<uint32_t, NVML::nvmlDevice_t> m_DeviceHandles;
};

} // namespace Platform
