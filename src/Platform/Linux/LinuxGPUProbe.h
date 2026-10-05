#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
#include <vector>

namespace Platform
{

class NVMLGPUProbe;
class DRMGPUProbe;
class ROCmGPUProbe;

/// Composite Linux GPU probe that delegates to vendor-specific probes.
/// Phase 4: Uses NVML for NVIDIA GPUs
/// Phase 5: Uses DRM for Intel GPUs
/// Phase 6: Uses ROCm for AMD GPUs
class LinuxGPUProbe : public IGPUProbe
{
  public:
    /// `drmBasePath` and `pciDevicesRoot` are the sysfs roots the vendor probes read; tests pass a
    /// fake tree. Each adapter's GPUInfo::sensorCapabilities says which of the OR'd capabilities()
    /// it actually reports (#1112).
    explicit LinuxGPUProbe(std::string drmBasePath = "/sys/class/drm", const std::string& pciDevicesRoot = "/sys/bus/pci/devices");
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
    std::unique_ptr<ROCmGPUProbe> m_ROCmProbe;
};

} // namespace Platform
