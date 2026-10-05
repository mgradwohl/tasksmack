#pragma once

#include "ComPtr.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declare DXGI interfaces to avoid including d3d headers in header
// NOLINTBEGIN(cppcoreguidelines-virtual-class-destructor) - COM interface forward decls
struct IDXGIFactory1;
struct IDXGIAdapter1;
// NOLINTEND(cppcoreguidelines-virtual-class-destructor)

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

    [[nodiscard]] static bool isIntegratedGPU(IDXGIAdapter1* adapter);

    /// Whether the adapter with these DXGI_ADAPTER_DESC1 flags and LUID is listed as a GPU (see
    /// shouldListAdapter()). enumerateGPUs() and readGPUCounters() both ask, so they skip the same
    /// adapters and the "GPU{index}" ids agree (#1251).
    [[nodiscard]] bool isListedAdapter(std::uint32_t flags, std::int32_t luidHighPart, std::uint32_t luidLowPart);

    ComPtr<IDXGIFactory1> m_Factory;
    bool m_Initialized{false};
    /// isListedAdapter()'s decision per adapter LUID, so the adapter-type query (which opens the
    /// kernel adapter) runs once per adapter rather than on every counter read (#1251).
    std::unordered_map<std::uint64_t, bool> m_ListedByLuid;
};

} // namespace Platform
