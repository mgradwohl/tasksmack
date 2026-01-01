#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <vector>

namespace Platform
{

class NVMLGPUProbe;
class DRMGPUProbe;

/// Composite Linux GPU probe that delegates to vendor-specific probes.
/// Phase 4: Uses NVML for NVIDIA GPUs
/// Phase 5: Uses DRM for Intel GPUs
/// Future phases will add ROCm (AMD)
class LinuxGPUProbe : public IGPUProbe
{
  public:
    LinuxGPUProbe();
    ~LinuxGPUProbe() override;

    // Rule of 5
    LinuxGPUProbe(const LinuxGPUProbe&) = delete;
    LinuxGPUProbe& operator=(const LinuxGPUProbe&) = delete;
    LinuxGPUProbe(LinuxGPUProbe&&) = delete;
    LinuxGPUProbe& operator=(LinuxGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

  private:
    std::unique_ptr<NVMLGPUProbe> m_NVMLProbe;
    std::unique_ptr<DRMGPUProbe> m_DRMProbe;
};

} // namespace Platform
