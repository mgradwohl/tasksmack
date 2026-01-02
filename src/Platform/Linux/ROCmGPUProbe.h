#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
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
    ROCmGPUProbe();
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

    /// Check if ROCm SMI is available and initialized
    [[nodiscard]] bool isAvailable() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
