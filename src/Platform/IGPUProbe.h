#pragma once

#include "GPUTypes.h"

#include <chrono>

namespace Platform
{

class IGPUProbe
{
  public:
    virtual ~IGPUProbe() = default;

    IGPUProbe() = default;
    IGPUProbe(const IGPUProbe&) = default;
    IGPUProbe& operator=(const IGPUProbe&) = default;
    IGPUProbe(IGPUProbe&&) = default;
    IGPUProbe& operator=(IGPUProbe&&) = default;

    // Enumerate available GPUs (called once at startup or on refresh)
    [[nodiscard]] virtual std::vector<GPUInfo> enumerateGPUs() = 0;

    // Read system-level GPU metrics (called every sample interval)
    [[nodiscard]] virtual std::vector<GPUCounters> readGPUCounters() = 0;

    // Read per-process GPU metrics (called every sample interval)
    // Returns empty vector if not supported
    [[nodiscard]] virtual std::vector<ProcessGPUCounters> readProcessGPUCounters() = 0;

    // Capability reporting
    [[nodiscard]] virtual GPUCapabilities capabilities() const = 0;

    // Optional configuration: Set PDH instance refresh interval (Windows-only)
    // This controls how often PDH refreshes the list of GPU-using processes.
    // Has no effect on Linux or other platforms.
    virtual void setInstanceRefreshInterval([[maybe_unused]] std::chrono::seconds interval) {}
};

} // namespace Platform
