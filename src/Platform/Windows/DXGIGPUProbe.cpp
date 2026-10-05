#include "DXGIGPUProbe.h"

#include "DXGIAdapterLocation.h"
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
#include <optional>

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

bool DXGIGPUProbe::isListedAdapter(std::uint32_t flags, std::int32_t luidHighPart, std::uint32_t luidLowPart)
{
    const std::uint64_t luidKey = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luidHighPart)) << 32U) | luidLowPart;
    if (const auto it = m_ListedByLuid.find(luidKey); it != m_ListedByLuid.end())
    {
        return it->second;
    }

    const bool softwareFlag = (flags & static_cast<std::uint32_t>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0U;
    std::optional<AdapterTypeBits> typeBits;
    if (!softwareFlag)
    {
        const LUID luid{.LowPart = luidLowPart, .HighPart = luidHighPart};
        if (const auto kind = adapterKind(luid))
        {
            typeBits = adapterTypeBits(*kind);
        }
    }
    const bool listed = shouldListAdapter(softwareFlag, typeBits);
    if (!listed)
    {
        spdlog::debug("DXGIGPUProbe: Skipping adapter LUID {} (software: {}, indirect display: {})",
                      luidToPdhFormat(static_cast<std::uint32_t>(luidHighPart), luidLowPart),
                      softwareFlag || (typeBits.has_value() && typeBits->softwareDevice),
                      typeBits.has_value() && typeBits->indirectDisplayDevice);
    }
    m_ListedByLuid.emplace(luidKey, listed);
    return listed;
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
            // Skip software adapters (WARP, etc.) and indirect-display adapters (#1251)
            if (isListedAdapter(
                    desc.Flags, static_cast<std::int32_t>(desc.AdapterLuid.HighPart), static_cast<std::uint32_t>(desc.AdapterLuid.LowPart)))
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

                // PCI identity, in NVML's pciDeviceId encoding, for matching to NVML (#1091)
                info.pciDeviceId = (static_cast<std::uint32_t>(desc.DeviceId) << 16U) | (desc.VendorId & 0xFFFFU);
                info.pciLocation = adapterPciLocation(desc.AdapterLuid);

                spdlog::debug("DXGIGPUProbe: Enumerated GPU {}: {} ({}) - LUID: {}, PCI: {}, Integrated: {}",
                              adapterIndex,
                              info.name,
                              info.vendor,
                              info.luidId,
                              info.pciLocation ? std::format("{:02x}:{:02x}", info.pciLocation->bus, info.pciLocation->device)
                                               : std::string("unknown"),
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
            // Skip the same adapters enumerateGPUs() does, so the "GPU{index}" ids match (#1251)
            if (isListedAdapter(
                    desc.Flags, static_cast<std::int32_t>(desc.AdapterLuid.HighPart), static_cast<std::uint32_t>(desc.AdapterLuid.LowPart)))
            {
                // The adapter's own memory size. Usage is not read here: QueryVideoMemoryInfo
                // reports the calling process's usage and budget, not the adapter's, so the GPU tab
                // used to chart TaskSmack's own few MB as the GPU's memory (#1029). WindowsGPUProbe
                // fills memoryUsedBytes from PDH's adapter-wide counters (or NVML); until then
                // utilization and memory are unread, not a real 0 (#1245).
                const bool integrated = isIntegratedGPUFromDesc(desc.VendorId, desc.Flags, desc.DedicatedVideoMemory);
                counters.push_back(
                    makeDXGIAdapterCounters(std::format("GPU{}", adapterIndex),
                                            adapterMemoryTotalBytes(integrated, desc.DedicatedVideoMemory, desc.SharedSystemMemory)));
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
