#pragma once

#include "GPUTypes.h"

#include <memory>
#include <vector>

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
};

} // namespace Platform
