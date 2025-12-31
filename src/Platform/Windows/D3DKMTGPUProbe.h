#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <string>
#include <vector>

namespace Platform
{

/// D3DKMT-based GPU probe for per-process GPU metrics on Windows.
/// Uses D3DKMTQueryStatistics for per-process GPU memory and engine usage.
/// Works with all GPU vendors (NVIDIA, AMD, Intel) via Windows kernel API.
class D3DKMTGPUProbe final : public IGPUProbe
{
  public:
    D3DKMTGPUProbe();
    ~D3DKMTGPUProbe() override = default;

    // Rule of 5: Delete copy/move operations
    D3DKMTGPUProbe(const D3DKMTGPUProbe&) = delete;
    D3DKMTGPUProbe& operator=(const D3DKMTGPUProbe&) = delete;
    D3DKMTGPUProbe(D3DKMTGPUProbe&&) = delete;
    D3DKMTGPUProbe& operator=(D3DKMTGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
