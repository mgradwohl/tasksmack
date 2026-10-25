#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
#include <vector>

namespace Platform
{

class NVMLGPUProbe;
class AccelNpuProbe;
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
    /// @param accelClassRoot where NPUs and compute accelerators are listed (#1827)
    explicit LinuxGPUProbe(const std::string& drmBasePath = "/sys/class/drm",
                           const std::string& pciDevicesRoot = "/sys/bus/pci/devices",
                           std::string accelClassRoot = "/sys/class/accel");
    /// The same, with @p drmProbe as the Intel DRM probe and @p amdClientsProbe as the one reading the
    /// amdgpu cards' clients: tests hand them a fake sysfs and /proc tree, so their per-process figures can
    /// be checked through the composite (#2892, #2589).
    LinuxGPUProbe(std::unique_ptr<DRMGPUProbe> drmProbe,
                  std::unique_ptr<DRMGPUProbe> amdClientsProbe,
                  const std::string& pciDevicesRoot,
                  std::string accelClassRoot);
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
    /// Rescans every vendor probe, available or not -- a probe with no device at startup can gain
    /// one (a hot-plugged eGPU) -- and reports whether any of them changed (#1116, #1289).
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

  private:
    std::unique_ptr<NVMLGPUProbe> m_NVMLProbe;
    std::unique_ptr<DRMGPUProbe> m_DRMProbe;
    std::unique_ptr<ROCmGPUProbe> m_ROCmProbe;
    /// The amdgpu cards' DRM clients, for ROCm's GPUs' per-process figures (#2589): ROCm SMI has none.
    /// Used only while ROCm SMI runs, since its processes carry ROCm's GPU ids.
    std::unique_ptr<DRMGPUProbe> m_AmdClientsProbe;

    /// Whether m_AmdClientsProbe has cards and ROCm SMI ids to label them with.
    [[nodiscard]] bool amdClientsActive() const;
    /// Hands m_AmdClientsProbe ROCm's ids for the cards, after ROCm (re)enumerates.
    void shareRocmIds();
    std::unique_ptr<AccelNpuProbe> m_AccelProbe; // NPUs and compute accelerators (#1827)
};

} // namespace Platform
