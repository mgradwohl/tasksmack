#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

/// ROCm-based GPU probe for AMD GPUs on Linux.
/// Uses ROCm SMI library (rocm_smi_lib) for AMD GPU metrics.
/// Supports dynamic loading of librocm_smi64.so for graceful degradation.
/// Provides system-level metrics; per-process GPU utilization not available via ROCm.
class ROCmGPUProbe final : public IGPUProbe
{
  public:
    /// A GPU that is runtime-suspended (its power/runtime_status under `pciDevicesRoot` says so) is
    /// not queried, so the probe doesn't keep an idle dGPU awake (#1117); tests pass a fake root.
    /// rescanGPUs() re-initialises ROCm SMI when the AMD devices under that root change (hot-plug,
    /// removal, a driver rebind), a read reported an initialisation error, or ROCm SMI failed to start
    /// while a GPU is bound to amdgpu (a driver mid-reload), and asks for a
    /// re-enumeration once a GPU asleep at enumeration wakes, so its own sensors are found (#1116, #1289).
    explicit ROCmGPUProbe(std::string pciDevicesRoot = "/sys/bus/pci/devices");
    ~ROCmGPUProbe() override;

    // Rule of 5: Delete copy/move operations
    ROCmGPUProbe(const ROCmGPUProbe&) = delete;
    ROCmGPUProbe& operator=(const ROCmGPUProbe&) = delete;
    ROCmGPUProbe(ROCmGPUProbe&&) = delete;
    ROCmGPUProbe& operator=(ROCmGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

    /// Check if ROCm SMI is available and initialized
    [[nodiscard]] bool isAvailable() const;

    /// Each device's PCI address ("0000:03:00.0") and the GPU id this probe gives it, for the DRM probe
    /// that reads the amdgpu cards' clients to label their processes with (#2589). A device whose
    /// address ROCm SMI didn't give is left out; empty while ROCm SMI isn't running.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> gpuIdsByPciAddress() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
