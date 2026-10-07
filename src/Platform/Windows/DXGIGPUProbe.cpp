#include "DXGIGPUProbe.h"

#include "ComPtr.h"
#include "DXGIAdapterLocation.h"
#include "DXGIGPUProbeMath.h"
#include "Platform/GPUTypes.h"
#include "WinString.h"
#include "WindowsProcAddress.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
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
#include <dxcore_interface.h>
#pragma clang diagnostic pop
// clang-format on

#include <format>
#include <optional>

namespace Platform
{

namespace
{

/// dxcore.dll's DXCoreCreateAdapterFactory, resolved at run time rather than linked, so TaskSmack
/// still starts on a Windows 10 that has no DXCore (#1263).
using DXCoreCreateAdapterFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

[[nodiscard]] std::uint64_t luidKey(std::int32_t luidHighPart, std::uint32_t luidLowPart)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(luidHighPart)) << 32U) | luidLowPart;
}

/// A DXGI factory from CreateDXGIFactory1(), or null if it can't be created.
[[nodiscard]] ComPtr<IDXGIFactory1> createSystemDXGIFactory()
{
    ComPtr<IDXGIFactory1> factory;
    // __uuidof is a Microsoft extension, suppress warning
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
    const HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.releaseAndGetAddressOf()));
#pragma clang diagnostic pop
    if (FAILED(hr))
    {
        spdlog::debug("DXGIGPUProbe: CreateDXGIFactory1 failed (HRESULT: 0x{:08X})", static_cast<uint32_t>(hr));
        return {};
    }
    return factory;
}

} // namespace

// DXCore says whether an adapter is integrated (#1263); load it from System32 only, like nvml.dll.
DXGIGPUProbe::DXGIGPUProbe()
    : m_CreateFactory(createSystemDXGIFactory),
      m_D3DKMT(std::make_unique<D3DKMTAdapterFunctions>()),
      m_Initialized(initialize()),
      m_DXCoreModule(LoadLibraryExW(L"dxcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
{
    if (m_DXCoreModule == nullptr)
    {
        spdlog::debug("DXGIGPUProbe: dxcore.dll not available; classifying adapters by their descriptor");
        return;
    }
    if (!createDXCoreFactory())
    {
        spdlog::debug("DXGIGPUProbe: DXCore adapter factory unavailable; classifying adapters by their descriptor");
        unloadDXCore();
    }
}

DXGIGPUProbe::~DXGIGPUProbe()
{
    unloadDXCore();
}

void DXGIGPUProbe::unloadDXCore()
{
    // The factory's code lives in dxcore.dll: release it before unloading the module.
    m_DXCoreFactory.reset();
    if (m_DXCoreModule != nullptr)
    {
        FreeLibrary(static_cast<HMODULE>(m_DXCoreModule));
        m_DXCoreModule = nullptr;
    }
}

bool DXGIGPUProbe::createDXCoreFactory()
{
    m_DXCoreFactory.reset();
    if (m_DXCoreModule == nullptr)
    {
        return false;
    }
    const auto createFactory =
        Windows::getProcAddress<DXCoreCreateAdapterFactoryFn>(static_cast<HMODULE>(m_DXCoreModule), "DXCoreCreateAdapterFactory");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
    const bool created =
        createFactory != nullptr &&
        SUCCEEDED(createFactory(__uuidof(IDXCoreAdapterFactory), reinterpret_cast<void**>(m_DXCoreFactory.releaseAndGetAddressOf()))) &&
        m_DXCoreFactory;
#pragma clang diagnostic pop
    if (!created)
    {
        m_DXCoreFactory.reset();
    }
    return created;
}

bool DXGIGPUProbe::initialize()
{
    // Create DXGI factory for GPU enumeration
    m_Factory = m_CreateFactory ? m_CreateFactory() : ComPtr<IDXGIFactory1>{};
    if (!m_Factory)
    {
        spdlog::warn("DXGIGPUProbe: Failed to create DXGI factory");
        return false;
    }

    spdlog::debug("DXGIGPUProbe: Successfully initialized");
    return true;
}

bool DXGIGPUProbe::rescanGPUs(GPURescan depth)
{
    // DXGI adapters don't sleep, so a quick rescan has nothing to find.
    if (depth != GPURescan::Full)
    {
        return false;
    }
    // A factory lists the adapters present when it was made; IsCurrent() turns false once one is
    // added or removed, or a driver is updated or reset (#1294).
    if (m_Factory && m_Factory->IsCurrent() != FALSE)
    {
        return false;
    }
    auto factory = m_CreateFactory ? m_CreateFactory() : ComPtr<IDXGIFactory1>{};
    if (!factory)
    {
        // Keep the factory we had: the adapters it lists are better than none. Retried next time.
        spdlog::debug("DXGIGPUProbe: Could not create a new DXGI factory; retrying at the next full rescan");
        return false;
    }
    spdlog::info("DXGIGPUProbe: {}; re-enumerating adapters",
                 m_Factory ? "The adapter set changed (DXGI factory no longer current)" : "DXGI factory created");
    m_Factory = std::move(factory);
    m_Initialized = true;
    // A driver update or reset brings an adapter back under a new LUID, and a LUID can be reused, so
    // what was decided per LUID is decided again as adapters are seen (#1251, #1263). DXCore's
    // factory is made afresh alongside, so it knows the new adapters too.
    m_ListedByLuid.clear();
    m_IntegratedByLuid.clear();
    m_IdByLuid.clear();
    if (m_DXCoreModule != nullptr && !createDXCoreFactory())
    {
        spdlog::debug("DXGIGPUProbe: DXCore adapter factory unavailable; classifying adapters by their descriptor");
    }
    return true;
}

std::optional<bool> DXGIGPUProbe::dxcoreIsIntegrated(std::int32_t luidHighPart, std::uint32_t luidLowPart)
{
    if (!m_DXCoreFactory)
    {
        return std::nullopt;
    }
    const LUID luid{.LowPart = luidLowPart, .HighPart = luidHighPart};
    ComPtr<IDXCoreAdapter> adapter;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
    const HRESULT hr =
        m_DXCoreFactory->GetAdapterByLuid(luid, __uuidof(IDXCoreAdapter), reinterpret_cast<void**>(adapter.releaseAndGetAddressOf()));
#pragma clang diagnostic pop
    if (FAILED(hr) || !adapter || !adapter->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated))
    {
        return std::nullopt;
    }
    bool integrated = false;
    if (FAILED(adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, sizeof(integrated), &integrated)))
    {
        return std::nullopt;
    }
    return integrated;
}

bool DXGIGPUProbe::isIntegratedAdapter(
    std::uint32_t vendorId, std::uint32_t flags, std::uint64_t dedicatedVideoMemory, std::int32_t luidHighPart, std::uint32_t luidLowPart)
{
    const std::uint64_t key = luidKey(luidHighPart, luidLowPart);
    if (const auto it = m_IntegratedByLuid.find(key); it != m_IntegratedByLuid.end())
    {
        return it->second;
    }
    const std::optional<bool> dxcore = dxcoreIsIntegrated(luidHighPart, luidLowPart);
    const bool integrated = classifyIntegrated(dxcore, vendorId, flags, dedicatedVideoMemory);
    spdlog::debug("DXGIGPUProbe: Adapter LUID {} is {} (by {})",
                  luidToPdhFormat(static_cast<std::uint32_t>(luidHighPart), luidLowPart),
                  integrated ? "integrated" : "discrete",
                  dxcore.has_value() ? "DXCore" : "descriptor heuristic");
    m_IntegratedByLuid.emplace(key, integrated);
    return integrated;
}

bool DXGIGPUProbe::isListedAdapter(std::uint32_t flags, std::int32_t luidHighPart, std::uint32_t luidLowPart)
{
    const std::uint64_t key = luidKey(luidHighPart, luidLowPart);
    if (const auto it = m_ListedByLuid.find(key); it != m_ListedByLuid.end())
    {
        return it->second;
    }

    const bool softwareFlag = (flags & static_cast<std::uint32_t>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0U;
    std::optional<AdapterTypeBits> typeBits;
    if (!softwareFlag)
    {
        const LUID luid{.LowPart = luidLowPart, .HighPart = luidHighPart};
        if (const auto kind = adapterKind(luid, *m_D3DKMT))
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
    m_ListedByLuid.emplace(key, listed);
    return listed;
}

std::string DXGIGPUProbe::adapterId(std::uint32_t vendorId,
                                    std::uint32_t deviceId,
                                    std::int32_t luidHighPart,
                                    std::uint32_t luidLowPart,
                                    const std::function<std::optional<PciLocation>()>& pciLocation)
{
    const std::uint64_t key = luidKey(luidHighPart, luidLowPart);
    if (const auto it = m_IdByLuid.find(key); it != m_IdByLuid.end())
    {
        return it->second;
    }
    std::unordered_set<std::string> taken;
    for (const auto& entry : m_IdByLuid)
    {
        taken.insert(entry.second);
    }
    std::string id = stableAdapterId(pciLocation(), vendorId, deviceId, static_cast<std::uint32_t>(luidHighPart), luidLowPart, taken);
    spdlog::debug("DXGIGPUProbe: Adapter LUID {} has id {}", luidToPdhFormat(static_cast<std::uint32_t>(luidHighPart), luidLowPart), id);
    m_IdByLuid.emplace(key, id);
    return id;
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

                // PCI identity, in NVML's pciDeviceId encoding, for matching to NVML (#1091)
                info.pciDeviceId = (static_cast<std::uint32_t>(desc.DeviceId) << 16U) | (desc.VendorId & 0xFFFFU);
                info.pciLocation = adapterPciLocation(desc.AdapterLuid, *m_D3DKMT);

                // The adapter's own id -- its PCI location, or its LUID without one -- not its place
                // in this list, which shifts when an adapter before it is removed (#1317)
                info.id = adapterId(desc.VendorId,
                                    desc.DeviceId,
                                    static_cast<std::int32_t>(desc.AdapterLuid.HighPart),
                                    static_cast<std::uint32_t>(desc.AdapterLuid.LowPart),
                                    [&info] { return info.pciLocation; });

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

                // Integrated or discrete: DXCore's answer where it has one (#1263)
                info.isIntegrated = isIntegratedAdapter(desc.VendorId,
                                                        desc.Flags,
                                                        desc.DedicatedVideoMemory,
                                                        static_cast<std::int32_t>(desc.AdapterLuid.HighPart),
                                                        static_cast<std::uint32_t>(desc.AdapterLuid.LowPart));
                info.memoryIsShared = adapterMemoryIsShared(info.isIntegrated); // What its used figure counts (#1164)

                // Position in DXGI's list: display order only, never identity (#1317)
                info.deviceIndex = adapterIndex;

                spdlog::debug("DXGIGPUProbe: Enumerated GPU {} ({}): {} ({}) - LUID: {}, PCI: {}, Integrated: {}",
                              adapterIndex,
                              info.id,
                              info.name,
                              info.vendor,
                              info.luidId,
                              info.pciLocation ? std::format("{:02x}:{:02x}.{}",
                                                             info.pciLocation->bus,
                                                             info.pciLocation->device,
                                                             info.pciLocation->function.value_or(0))
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
            // Skip the same adapters enumerateGPUs() does (#1251)
            if (isListedAdapter(
                    desc.Flags, static_cast<std::int32_t>(desc.AdapterLuid.HighPart), static_cast<std::uint32_t>(desc.AdapterLuid.LowPart)))
            {
                // The adapter's own memory size. Usage is not read here: QueryVideoMemoryInfo
                // reports the calling process's usage and budget, not the adapter's, so the GPU tab
                // used to chart TaskSmack's own few MB as the GPU's memory (#1029). WindowsGPUProbe
                // fills memoryUsedBytes from PDH's adapter-wide counters (or NVML); until then
                // utilization and memory are unread, not a real 0 (#1245).
                // The classification enumerateGPUs() reported, so the pool matches the label (#1263)
                const bool integrated = isIntegratedAdapter(desc.VendorId,
                                                            desc.Flags,
                                                            desc.DedicatedVideoMemory,
                                                            static_cast<std::int32_t>(desc.AdapterLuid.HighPart),
                                                            static_cast<std::uint32_t>(desc.AdapterLuid.LowPart));
                // The id enumerateGPUs() gave this adapter, so counters and history agree (#1317)
                const LUID luid = desc.AdapterLuid;
                std::string id = adapterId(desc.VendorId,
                                           desc.DeviceId,
                                           static_cast<std::int32_t>(luid.HighPart),
                                           static_cast<std::uint32_t>(luid.LowPart),
                                           [this, &luid] { return adapterPciLocation(luid, *m_D3DKMT); });
                counters.push_back(makeDXGIAdapterCounters(
                    std::move(id), adapterMemoryTotalBytes(integrated, desc.DedicatedVideoMemory, desc.SharedSystemMemory)));
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
    caps.hasPerProcessUtilization = false;
    caps.hasEncoderDecoder = false; // No encoder/decoder via DXGI
    caps.supportsMultiGPU = true;   // DXGI supports enumerating multiple GPUs

    return caps;
}

} // namespace Platform
