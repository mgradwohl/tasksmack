#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
#include <vector>

namespace Platform
{

/// NVML-based GPU probe for NVIDIA GPUs on Linux.
/// Uses NVIDIA Management Library (NVML) for comprehensive GPU metrics.
/// Supports dynamic loading of libnvidia-ml.so for graceful degradation.
/// A GPU that is runtime-suspended (its power/runtime_status under `pciDevicesRoot` says so) is not
/// queried, so the probe doesn't keep a hybrid laptop's dGPU awake (#1117); tests pass a fake root.
class NVMLGPUProbe final : public IGPUProbe
{
  public:
    explicit NVMLGPUProbe(std::string pciDevicesRoot = "/sys/bus/pci/devices");
    ~NVMLGPUProbe() override;

    // Rule of 5: Delete copy/move operations
    NVMLGPUProbe(const NVMLGPUProbe&) = delete;
    NVMLGPUProbe& operator=(const NVMLGPUProbe&) = delete;
    NVMLGPUProbe(NVMLGPUProbe&&) = delete;
    NVMLGPUProbe& operator=(NVMLGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

    /// Check if NVML is available and initialized
    [[nodiscard]] bool isAvailable() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
