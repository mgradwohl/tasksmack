#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <vector>

namespace Platform
{

/// Stub Windows GPU probe that returns no GPUs (placeholder for future implementation).
class WindowsGPUProbe : public IGPUProbe
{
  public:
    WindowsGPUProbe() = default;
    ~WindowsGPUProbe() override = default;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
};

} // namespace Platform
