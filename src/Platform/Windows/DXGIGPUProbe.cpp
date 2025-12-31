#include "DXGIGPUProbe.h"

#include <spdlog/spdlog.h>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
// clang-format on

#include <array>
#include <cstring>
#include <format>

namespace Platform
{

namespace
{

/// Convert vendor ID to vendor name string
[[nodiscard]] std::string vendorIdToName(uint32_t vendorId)
{
    switch (vendorId)
    {
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
    case 0x1022:
        return "AMD";
    case 0x8086:
    case 0x8087:
        return "Intel";
    default:
        return "Unknown";
    }
}

} // namespace

DXGIGPUProbe::DXGIGPUProbe()
{
    m_Initialized = initialize();
}

DXGIGPUProbe::~DXGIGPUProbe()
{
    cleanup();
}

bool DXGIGPUProbe::initialize()
{
    // Create DXGI factory for GPU enumeration
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&m_Factory));
    if (FAILED(hr) || m_Factory == nullptr)
    {
        spdlog::warn("DXGIGPUProbe: Failed to create DXGI factory (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
        return false;
    }

    spdlog::debug("DXGIGPUProbe: Successfully initialized");
    return true;
}

void DXGIGPUProbe::cleanup()
{
    if (m_Factory != nullptr)
    {
        m_Factory->Release();
        m_Factory = nullptr;
    }
}

std::string DXGIGPUProbe::wcharToUtf8(const wchar_t* wstr) const
{
    if (wstr == nullptr || wstr[0] == L'\0')
    {
        return {};
    }

    // Get required buffer size
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    // Convert to UTF-8
    std::string result(static_cast<std::size_t>(size) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), size, nullptr, nullptr);
    return result;
}

bool DXGIGPUProbe::isIntegratedGPU(IDXGIAdapter1* adapter) const
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

    // Check if this is a software adapter or integrated GPU
    // DXGI_ADAPTER_FLAG_SOFTWARE = 0x2
    constexpr UINT SOFTWARE_FLAG = 2;
    if ((desc.Flags & SOFTWARE_FLAG) != 0)
    {
        return false; // Skip software adapters
    }

    // Intel integrated GPUs typically have vendor ID 0x8086
    // Intel UHD/Iris integrated graphics have lower dedicated video memory
    if (desc.VendorId == 0x8086)
    {
        // Intel GPUs with < 512MB dedicated VRAM are likely integrated
        return desc.DedicatedVideoMemory < (512ULL * 1024 * 1024);
    }

    // AMD APUs (integrated) have vendor ID 0x1002 but lower dedicated memory
    if (desc.VendorId == 0x1002)
    {
        // AMD integrated GPUs typically have < 1GB dedicated VRAM
        return desc.DedicatedVideoMemory < (1024ULL * 1024 * 1024);
    }

    // NVIDIA doesn't make consumer integrated GPUs (Tegra is different architecture)
    // Assume discrete for NVIDIA
    return false;
}

std::vector<GPUInfo> DXGIGPUProbe::enumerateGPUs()
{
    std::vector<GPUInfo> gpus;

    if (!m_Initialized || m_Factory == nullptr)
    {
        return gpus;
    }

    // Enumerate all adapters
    UINT adapterIndex = 0;
    IDXGIAdapter1* adapter = nullptr;

    while (m_Factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        if (adapter == nullptr)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        HRESULT hr = adapter->GetDesc1(&desc);

        if (SUCCEEDED(hr))
        {
            // Skip software adapters (WARP, etc.)
            constexpr UINT SOFTWARE_FLAG = 2;
            if ((desc.Flags & SOFTWARE_FLAG) == 0)
            {
                GPUInfo info{};

                // Generate unique ID from LUID
                info.id = std::format("GPU{}", adapterIndex);

                // Convert name from wide char
                info.name = wcharToUtf8(desc.Description);

                // Determine vendor
                info.vendor = vendorIdToName(desc.VendorId);

                // Driver version (encoded in DriverVersion LARGE_INTEGER)
                // Windows encodes driver version as: Product.Version.SubVersion.Build
                LARGE_INTEGER driverVer = desc.DriverVersion;
                uint64_t ver = static_cast<uint64_t>(driverVer.QuadPart);
                uint32_t product = static_cast<uint32_t>((ver >> 48) & 0xFFFF);
                uint32_t version = static_cast<uint32_t>((ver >> 32) & 0xFFFF);
                uint32_t subVersion = static_cast<uint32_t>((ver >> 16) & 0xFFFF);
                uint32_t build = static_cast<uint32_t>(ver & 0xFFFF);
                info.driverVersion = std::format("{}.{}.{}.{}", product, version, subVersion, build);

                // Determine if integrated
                info.isIntegrated = isIntegratedGPU(adapter);

                // Device index
                info.deviceIndex = adapterIndex;

                gpus.push_back(std::move(info));

                spdlog::debug("DXGIGPUProbe: Enumerated GPU {}: {} ({}) - Driver: {}, Integrated: {}",
                              adapterIndex,
                              info.name,
                              info.vendor,
                              info.driverVersion,
                              info.isIntegrated);
            }
        }

        adapter->Release();
        ++adapterIndex;
    }

    spdlog::info("DXGIGPUProbe: Enumerated {} GPU(s)", gpus.size());
    return gpus;
}

std::vector<GPUCounters> DXGIGPUProbe::readGPUCounters()
{
    std::vector<GPUCounters> counters;

    if (!m_Initialized || m_Factory == nullptr)
    {
        return counters;
    }

    // Enumerate adapters and read memory info
    UINT adapterIndex = 0;
    IDXGIAdapter1* adapter = nullptr;

    while (m_Factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND)
    {
        if (adapter == nullptr)
        {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        HRESULT hr = adapter->GetDesc1(&desc);

        if (SUCCEEDED(hr))
        {
            // Skip software adapters
            constexpr UINT SOFTWARE_FLAG = 2;
            if ((desc.Flags & SOFTWARE_FLAG) == 0)
            {
                GPUCounters counter{};
                counter.gpuId = std::format("GPU{}", adapterIndex);

                // Try to get IDXGIAdapter3 for QueryVideoMemoryInfo (Windows 10+)
                IDXGIAdapter3* adapter3 = nullptr;
                hr = adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));

                if (SUCCEEDED(hr) && adapter3 != nullptr)
                {
                    DXGI_QUERY_VIDEO_MEMORY_INFO memInfo{};
                    hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);

                    if (SUCCEEDED(hr))
                    {
                        counter.memoryUsedBytes = memInfo.CurrentUsage;
                        counter.memoryTotalBytes = memInfo.Budget;

                        // Calculate memory utilization percentage
                        if (memInfo.Budget > 0)
                        {
                            counter.memoryUtilPercent =
                                (static_cast<double>(memInfo.CurrentUsage) / static_cast<double>(memInfo.Budget)) * 100.0;
                        }
                    }

                    adapter3->Release();
                }
                else
                {
                    // Fallback: use dedicated memory size from adapter desc
                    counter.memoryTotalBytes = desc.DedicatedVideoMemory;
                    // Cannot determine current usage without QueryVideoMemoryInfo
                    counter.memoryUsedBytes = 0;
                    counter.memoryUtilPercent = 0.0;
                }

                counters.push_back(std::move(counter));
            }
        }

        adapter->Release();
        ++adapterIndex;
    }

    return counters;
}

std::vector<ProcessGPUCounters> DXGIGPUProbe::readProcessGPUCounters()
{
    // DXGI does not provide per-process GPU metrics
    // This will be implemented in D3DKMTGPUProbe (Phase 3)
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
