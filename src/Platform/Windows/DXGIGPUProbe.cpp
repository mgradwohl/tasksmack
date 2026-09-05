#include "DXGIGPUProbe.h"

#include "DXGIGPUProbeMath.h"
#include "Platform/GPUTypes.h"
#include "WinString.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Suppress __uuidof extension warning for DXGI
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <dxgi1_4.h>
#pragma clang diagnostic pop
// clang-format on

#include <cstring>
#include <format>

namespace Platform
{

DXGIGPUProbe::DXGIGPUProbe() : m_Initialized(initialize())
{}

DXGIGPUProbe::~DXGIGPUProbe() = default;

bool DXGIGPUProbe::initialize()
{
    // Create DXGI factory for GPU enumeration
    // __uuidof is a Microsoft extension, suppress warning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(m_Factory.releaseAndGetAddressOf()));
#pragma clang diagnostic pop
    if (FAILED(hr) || !m_Factory)
    {
        spdlog::warn("DXGIGPUProbe: Failed to create DXGI factory (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }

    spdlog::debug("DXGIGPUProbe: Successfully initialized");
    return true;
}

bool DXGIGPUProbe::isIntegratedGPU(IDXGIAdapter1* adapter)
{
    if (adapter == nullptr)
    {
        return false;
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)))
    {
        return false;
    }

    return isIntegratedGPUFromDesc(desc.VendorId, desc.Flags, desc.DedicatedVideoMemory);
}

std::vector<GPUInfo> DXGIGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    if (!m_Initialized || !m_Factory)
    {
        return gpus;
    }

    // Enumerate all adapters
    UINT adapterIndex = 0;
    ComPtr<IDXGIAdapter1> adapter;

    while (m_Factory->EnumAdapters1(adapterIndex, adapter.releaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND)
    {
        if (!adapter)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        const HRESULT hr = adapter->GetDesc1(&desc);

        if (SUCCEEDED(hr))
        {
            // Skip software adapters (WARP, etc.)
            constexpr UINT SOFTWARE_FLAG = 2;
            if ((desc.Flags & SOFTWARE_FLAG) == 0)
            {
                GPUInfo info{};

                // Generate unique ID from adapter index
                info.id = std::format("GPU{}", adapterIndex);

                // Generate LUID-based ID for PDH counter matching
                // PDH GPU counters use LUID format: GPU_0x{HighPart}_0x{LowPart}
                info.luidId =
                    luidToPdhFormat(static_cast<uint32_t>(desc.AdapterLuid.HighPart), static_cast<uint32_t>(desc.AdapterLuid.LowPart));

                // Convert name from wide char
                info.name = WinString::wideToUtf8(desc.Description);

                // Determine vendor
                info.vendor = vendorIdToName(desc.VendorId);

                // Driver version not available in DXGI_ADAPTER_DESC1
                info.driverVersion = "Unknown";

                // Determine if integrated
                info.isIntegrated = isIntegratedGPU(adapter.get());

                // Device index
                info.deviceIndex = adapterIndex;

                spdlog::debug("DXGIGPUProbe: Enumerated GPU {}: {} ({}) - LUID: {}, Integrated: {}",
                              adapterIndex,
                              info.name,
                              info.vendor,
                              info.luidId,
                              info.isIntegrated);

                gpus.push_back(std::move(info));
            }
        }

        ++adapterIndex;
    }

    spdlog::info("DXGIGPUProbe: Enumerated {} GPU(s)", gpus.size());
    return gpus;
}

std::vector<GPUCounters> DXGIGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    if (!m_Initialized || !m_Factory)
    {
        return counters;
    }

    // Enumerate adapters and read memory info
    UINT adapterIndex = 0;
    ComPtr<IDXGIAdapter1> adapter;

    while (m_Factory->EnumAdapters1(adapterIndex, adapter.releaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND)
    {
        if (!adapter)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        const HRESULT hrDesc = adapter->GetDesc1(&desc);

        if (SUCCEEDED(hrDesc))
        {
            // Skip software adapters
            constexpr UINT SOFTWARE_FLAG = 2;
            if ((desc.Flags & SOFTWARE_FLAG) == 0)
            {
                GPUCounters counter{};
                counter.gpuId = std::format("GPU{}", adapterIndex);

                // Try to get IDXGIAdapter3 for QueryVideoMemoryInfo (Windows 10+)
                ComPtr<IDXGIAdapter3> adapter3;
                // __uuidof is a Microsoft extension, suppress warning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
                const HRESULT hrQuery =
                    adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(adapter3.releaseAndGetAddressOf()));
#pragma clang diagnostic pop

                if (SUCCEEDED(hrQuery) && adapter3)
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                    const HRESULT hrMemInfo = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);

                    if (SUCCEEDED(hrMemInfo))
                    {
                        counter.memoryUsedBytes = memInfo.CurrentUsage;
                        counter.memoryTotalBytes = memInfo.Budget;
                    }
                }
                else
                {
                    // Fallback: use dedicated memory size from adapter desc
                    counter.memoryTotalBytes = desc.DedicatedVideoMemory;
                    // Cannot determine current usage without QueryVideoMemoryInfo
                    counter.memoryUsedBytes = 0;
                }

                counters.push_back(std::move(counter));
            }
        }

        ++adapterIndex;
    }

    return counters;
}

std::vector<ProcessGPUCounters> DXGIGPUProbe::readProcessGPUCounters()
{
    // DXGI does not provide per-process GPU metrics
    // Per-process GPU utilization requires PDH Performance Counters (GPU Engine)
    // or vendor-specific APIs (NVML for NVIDIA)
    return {};
}

GPUCapabilities DXGIGPUProbe::capabilities() const
{
    GPUCapabilities caps{};

    if (!m_Initialized)
    {
        return caps;
    }

    // DXGI provides basic capabilities
    caps.hasTemperature = false;       // No temperature via DXGI
    caps.hasHotspotTemp = false;       // No hotspot temp via DXGI
    caps.hasPowerMetrics = false;      // No power metrics via DXGI
    caps.hasClockSpeeds = false;       // No clock speeds via DXGI
    caps.hasFanSpeed = false;          // No fan speed via DXGI
    caps.hasPCIeMetrics = false;       // No PCIe metrics via DXGI
    caps.hasEngineUtilization = false; // No engine utilization via DXGI
    caps.hasPerProcessMetrics = false; // No per-process metrics via DXGI
    caps.hasEncoderDecoder = false;    // No encoder/decoder via DXGI
    caps.supportsMultiGPU = true;      // DXGI supports enumerating multiple GPUs

    return caps;
}

} // namespace Platform
