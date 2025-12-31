#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declare NVML types to avoid including nvml.h in header
using nvmlDevice_t = struct nvmlDevice_st*;
using nvmlReturn_t = int;

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
    bool loadNVML();
    void unloadNVML();
    bool initializeNVML();
    void shutdownNVML();

    [[nodiscard]] std::string getNVMLErrorString(nvmlReturn_t result) const;

    // NVML function pointers (dynamically loaded)
    struct NVMLFunctions
    {
        nvmlReturn_t (*Init)();
        nvmlReturn_t (*Shutdown)();
        nvmlReturn_t (*DeviceGetCount)(unsigned int*);
        nvmlReturn_t (*DeviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
        nvmlReturn_t (*DeviceGetName)(nvmlDevice_t, char*, unsigned int);
        nvmlReturn_t (*DeviceGetUUID)(nvmlDevice_t, char*, unsigned int);
        nvmlReturn_t (*DeviceGetMemoryInfo)(nvmlDevice_t, void*);
        nvmlReturn_t (*DeviceGetTemperature)(nvmlDevice_t, int, unsigned int*);
        nvmlReturn_t (*DeviceGetPowerUsage)(nvmlDevice_t, unsigned int*);
        nvmlReturn_t (*DeviceGetPowerManagementLimit)(nvmlDevice_t, unsigned int*);
        nvmlReturn_t (*DeviceGetClockInfo)(nvmlDevice_t, int, unsigned int*);
        nvmlReturn_t (*DeviceGetMaxClockInfo)(nvmlDevice_t, int, unsigned int*);
        nvmlReturn_t (*DeviceGetUtilizationRates)(nvmlDevice_t, void*);
        nvmlReturn_t (*DeviceGetPcieThroughput)(nvmlDevice_t, int, unsigned int*);
        nvmlReturn_t (*DeviceGetDriverVersion)(char*, unsigned int);
        nvmlReturn_t (*DeviceGetVbiosVersion)(nvmlDevice_t, char*, unsigned int);
        nvmlReturn_t (*DeviceGetFanSpeed)(nvmlDevice_t, unsigned int*);
    };

    void* m_NVMLHandle{nullptr};
    NVMLFunctions m_NVML{};
    bool m_Initialized{false};

    // Map device index to NVML handle
    std::unordered_map<uint32_t, nvmlDevice_t> m_DeviceHandles;
};

} // namespace Platform
