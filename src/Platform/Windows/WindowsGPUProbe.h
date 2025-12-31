#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <vector>

namespace Platform
{

class DXGIGPUProbe;

/// Composite Windows GPU probe that delegates to vendor-specific probes.
/// Phase 2: Uses DXGI for basic GPU enumeration (all vendors)
/// Future phases will add NVML (NVIDIA), D3DKMT (per-process), etc.
class WindowsGPUProbe : public IGPUProbe
{
  public:
    WindowsGPUProbe();
    ~WindowsGPUProbe() override = default;

    // Rule of 5
    WindowsGPUProbe(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe& operator=(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe(WindowsGPUProbe&&) = delete;
    WindowsGPUProbe& operator=(WindowsGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

  private:
    std::unique_ptr<DXGIGPUProbe> m_DXGIProbe;
};

} // namespace Platform
