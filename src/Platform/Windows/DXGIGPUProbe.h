#pragma once

#include "ComPtr.h"
#include "Platform/GPUTypes.h"
#include "Platform/IGPUProbe.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declare DXGI interfaces to avoid including d3d headers in header
// NOLINTBEGIN(cppcoreguidelines-virtual-class-destructor) - COM interface forward decls
struct IDXGIFactory1;
struct IDXCoreAdapterFactory;
// NOLINTEND(cppcoreguidelines-virtual-class-destructor)

namespace Platform
{

struct D3DKMTAdapterFunctions;

/// Windows DXGI GPU probe for basic GPU enumeration and memory metrics.
/// Works with all GPU vendors (NVIDIA, AMD, Intel).
/// Uses DXGI (DirectX Graphics Infrastructure) for GPU enumeration and memory info.
/// A DXGI factory lists the adapters present when it was created, so a full rescan replaces it once
/// IDXGIFactory1::IsCurrent() says the adapter set has changed (an adapter added or removed, a driver
/// updated or reset), and drops what it had decided per adapter LUID (#1294).
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
    /// Full: replace the DXGI factory when it is no longer current (or was never created) and report
    /// a change, so the next enumerateGPUs() lists the adapters present now. Quick: no change -- DXGI
    /// adapters don't sleep, so there is nothing to find between full rescans (#1294).
    [[nodiscard]] bool rescanGPUs(GPURescan depth) override;

  private:
    // Test-only: substitutes a fake DXGI factory and fake D3DKMT calls (tests/Mocks/WindowsDXGIFake.h).
    friend struct DXGIGPUProbeTestAccessor;

    /// Makes a DXGI factory: CreateDXGIFactory1() in production, a fake in tests. Null on failure.
    using FactoryCreator = std::function<ComPtr<IDXGIFactory1>()>;

    bool initialize();
    /// Create m_DXCoreFactory from the loaded dxcore.dll. False (and no factory) when it can't.
    bool createDXCoreFactory();
    /// Release the DXCore factory and unload dxcore.dll; adapters are then classified by descriptor.
    void unloadDXCore();

    /// Whether the adapter with this LUID is integrated: DXCore's answer, or the descriptor
    /// heuristic without one (see classifyIntegrated()). Decided once per LUID, so enumerateGPUs()
    /// and readGPUCounters() -- which picks the memory pool from it -- always agree (#1263).
    [[nodiscard]] bool isIntegratedAdapter(std::uint32_t vendorId,
                                           std::uint32_t flags,
                                           std::uint64_t dedicatedVideoMemory,
                                           std::int32_t luidHighPart,
                                           std::uint32_t luidLowPart);

    /// DXCore's DXCoreAdapterProperty::IsIntegrated for the adapter with this LUID, or nullopt when
    /// DXCore is unavailable (dxcore.dll is loaded at run time, so a Windows 10 without it still
    /// runs) or can't answer for that adapter (#1263).
    [[nodiscard]] std::optional<bool> dxcoreIsIntegrated(std::int32_t luidHighPart, std::uint32_t luidLowPart);

    /// Whether the adapter with these DXGI_ADAPTER_DESC1 flags and LUID is listed as a GPU (see
    /// shouldListAdapter()). enumerateGPUs() and readGPUCounters() both ask, so they skip the same
    /// adapters and the "GPU{index}" ids agree (#1251).
    [[nodiscard]] bool isListedAdapter(std::uint32_t flags, std::int32_t luidHighPart, std::uint32_t luidLowPart);

    FactoryCreator m_CreateFactory;
    /// The kernel-mode adapter calls behind the PCI location and adapter-type queries (gdi32's, or
    /// fakes in tests).
    std::unique_ptr<D3DKMTAdapterFunctions> m_D3DKMT;
    ComPtr<IDXGIFactory1> m_Factory;
    bool m_Initialized{false};
    /// isListedAdapter()'s decision per adapter LUID, so the adapter-type query (which opens the
    /// kernel adapter) runs once per adapter rather than on every counter read (#1251).
    std::unordered_map<std::uint64_t, bool> m_ListedByLuid;
    /// isIntegratedAdapter()'s decision per adapter LUID (#1263).
    std::unordered_map<std::uint64_t, bool> m_IntegratedByLuid;
    /// dxcore.dll and its adapter factory: the module is loaded once at construction, and the factory
    /// created then and again whenever the DXGI factory is replaced (#1294); null when unavailable.
    /// The module stays loaded while the factory lives (#1263).
    void* m_DXCoreModule{nullptr};
    ComPtr<IDXCoreAdapterFactory> m_DXCoreFactory;
};

} // namespace Platform
