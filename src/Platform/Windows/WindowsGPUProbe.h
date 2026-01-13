#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Platform
{

class DXGIGPUProbe;
class NVMLGPUProbe;
class PDHGPUProbe;

/// Composite Windows GPU probe that delegates to vendor-specific probes.
/// - DXGI: Basic GPU enumeration (all vendors)
/// - NVML: NVIDIA-specific metrics (temp, power, clocks, system utilization)
/// - PDH: Per-process GPU utilization and memory via Performance Counters (all vendors)
class WindowsGPUProbe : public IGPUProbe
{
  public:
    WindowsGPUProbe();
    ~WindowsGPUProbe() override;

    // Rule of 5
    WindowsGPUProbe(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe& operator=(const WindowsGPUProbe&) = delete;
    WindowsGPUProbe(WindowsGPUProbe&&) = delete;
    WindowsGPUProbe& operator=(WindowsGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;
    void setInstanceRefreshInterval(std::chrono::seconds interval) override;

  private:
    void mergeNVMLEnhancements(std::vector<GPUCounters>& dxgiCounters);
    void mergePDHSystemWideUtilization(std::vector<GPUCounters>& dxgiCounters);

    std::unique_ptr<DXGIGPUProbe> m_DXGIProbe;
    std::unique_ptr<NVMLGPUProbe> m_NVMLProbe;
    std::unique_ptr<PDHGPUProbe> m_PDHProbe;

    // Map DXGI GPU index to NVML GPU index (for merging data)
    std::unordered_map<uint32_t, uint32_t> m_DXGIToNVMLMap;
};

} // namespace Platform
