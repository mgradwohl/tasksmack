#pragma once

#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <string>
#include <vector>

// Forward declare DXGI interfaces to avoid including d3d headers in header
struct IDXGIFactory1;
struct IDXGIAdapter1;

namespace Platform
{

/// Windows DXGI GPU probe for basic GPU enumeration and memory metrics.
/// Works with all GPU vendors (NVIDIA, AMD, Intel).
/// Uses DXGI (DirectX Graphics Infrastructure) for GPU enumeration and memory info.
class DXGIGPUProbe : public IGPUProbe
{
  public:
    DXGIGPUProbe();
    ~DXGIGPUProbe() override;

    // Rule of 5
    DXGIGPUProbe(const DXGIGPUProbe&) = delete;
    DXGIGPUProbe& operator=(const DXGIGPUProbe&) = delete;
    DXGIGPUProbe(DXGIGPUProbe&&) = delete;
    DXGIGPUProbe& operator=(DXGIGPUProbe&&) = delete;

    [[nodiscard]] std::vector<GPUInfo> enumerateGPUs() override;
    [[nodiscard]] std::vector<GPUCounters> readGPUCounters() override;
    [[nodiscard]] std::vector<ProcessGPUCounters> readProcessGPUCounters() override;
    [[nodiscard]] GPUCapabilities capabilities() const override;

  private:
    bool initialize();
    void cleanup();

    [[nodiscard]] static std::string wcharToUtf8(const wchar_t* wstr);
    [[nodiscard]] static bool isIntegratedGPU(IDXGIAdapter1* adapter);

    IDXGIFactory1* m_Factory{nullptr};
    bool m_Initialized{false};
};

} // namespace Platform
