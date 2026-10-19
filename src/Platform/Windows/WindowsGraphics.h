#pragma once

// The Windows Graphics & displays facts (#1519), all unprivileged:
// - Adapters from DXGI (description, vendor and device ids, dedicated and shared memory, LUID), filtered as
//   the GPU tab filters them (shouldListAdapter(): no software or indirect-display adapters), with the PCI
//   location from D3DKMT (DXGIAdapterLocation.h). The driver version and date come from the display-class
//   device node with the same PCI location, or else the same vendor and device ids (SetupAPI's
//   DEVPKEY_Device_DriverVersion and DEVPKEY_Device_DriverDate).
// - Monitors from every adapter's DXGI outputs (indirect-display ones included): the desktop rectangle,
//   the colour space and bits per colour (IDXGIOutput6::GetDesc1), and the EDID from the monitor's device
//   registry key (Platform/EdidParser.h).
// - Each listed adapter's highest Direct3D feature level (#1636), from a D3D12 device made on it for the
//   query and released right after; d3d12.dll is loaded from System32 at run time, so a system without
//   it leaves the level empty.
// - The Vulkan loader's instance version and each listed adapter's Vulkan API and driver versions (#1635),
//   from one instance per read (Platform/VulkanInfo.h), matched to the adapter by LUID (by vendor and
//   device id for a device that reports none); vulkan-1.dll is loaded from System32 at run time, so a
//   system without it leaves them empty.
// Every call goes through an injectable table; WindowsSystemInfoProbe.cpp supplies the real one, tests
// substitute fakes.

#include "Platform/EdidParser.h"
#include "Platform/GPUTypes.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/VulkanInfo.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/WinString.h"
#include "Platform/Windows/WindowsConversions.h"
#include "Platform/Windows/WindowsDriverNodes.h" // DriverNode, findDriverNode()

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#include <dxgi1_6.h>
#include <d3dcommon.h> // D3D_FEATURE_LEVEL
#pragma clang diagnostic pop
// clang-format on

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Platform::WindowsGraphics
{

/// One DXGI output, as listAdapters() reads it.
struct OutputRecord
{
    std::wstring deviceName; ///< "\\.\DISPLAY1"
    RECT desktop{};
    bool hasDesc1 = false; ///< IDXGIOutput6::GetDesc1 answered: colorSpace and bitsPerColor are set
    DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT bitsPerColor = 0;
};

/// One DXGI adapter and its outputs.
struct AdapterRecord
{
    DXGI_ADAPTER_DESC1 desc{};
    std::vector<OutputRecord> outputs;
};

/// The calls readGraphics() makes.
struct Functions
{
    std::optional<std::vector<AdapterRecord>> (*listAdapters)() = nullptr; ///< nullopt when DXGI can't be used
    std::optional<AdapterTypeBits> (*adapterType)(const LUID& luid) = nullptr;
    std::optional<PciLocation> (*pciLocation)(const LUID& luid) = nullptr;
    std::vector<DriverNode> (*listDisplayDrivers)() = nullptr;
    std::vector<std::uint8_t> (*readMonitorEdid)(const std::wstring& outputDeviceName) = nullptr; ///< Empty when unreadable
    /// The adapter's highest Direct3D 12 feature level, from a device created for the query and released
    /// before it returns; nullopt when d3d12.dll is missing, the adapter isn't found or no device can be made.
    std::optional<D3D_FEATURE_LEVEL> (*maxFeatureLevel)(const LUID& luid) = nullptr;
    /// The Vulkan loader's versions and devices, from an instance created for the read and destroyed
    /// before it returns; nullopt when vulkan-1.dll is missing.
    std::optional<Vulkan::Info> (*readVulkan)() = nullptr;
};

/// The Vulkan device of the adapter with @p luid: one reporting that LUID, else one that reports no LUID
/// and has @p vendorId and @p deviceId; a device with another LUID never matches. Among those, the
/// adapter's own driver comes before Dozen, which layers Vulkan over Direct3D 12 on the same adapter and
/// reports its LUID too; then the first listed. Null when none matches.
[[nodiscard]] inline const Vulkan::Device*
findVulkanDevice(std::span<const Vulkan::Device> devices, const LUID& luid, std::uint32_t vendorId, std::uint32_t deviceId)
{
    const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(LUID)>>(luid);
    const Vulkan::Device* best = nullptr;
    int bestRank = 4; // 0: LUID, native; 1: LUID, Dozen; 2: ids, native; 3: ids, Dozen
    for (const Vulkan::Device& device : devices)
    {
        int rank = 0;
        if (device.luid.has_value())
        {
            if (*device.luid != bytes)
            {
                continue;
            }
        }
        else if (device.vendorId == vendorId && device.deviceId == deviceId)
        {
            rank = 2;
        }
        else
        {
            continue;
        }
        rank += device.driverId == Vulkan::DRIVER_ID_MESA_DOZEN ? 1 : 0;
        if (rank < bestRank)
        {
            best = &device;
            bestRank = rank;
        }
    }
    return best;
}

/// A Direct3D feature level as dxdiag writes it: D3D_FEATURE_LEVEL_12_2 is "12_2".
[[nodiscard]] inline std::string featureLevelName(D3D_FEATURE_LEVEL level)
{
    const auto value = static_cast<std::uint32_t>(level);
    return std::format("{}_{}", (value >> 12U) & 0xFU, (value >> 8U) & 0xFU);
}

/// A DXGI colour space's short name.
[[nodiscard]] inline std::string colorSpaceName(DXGI_COLOR_SPACE_TYPE space)
{
    switch (space)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
        return "sRGB";
    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        return "scRGB";
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020:
        return "BT.2020 PQ";
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
        return "BT.2020";
    default:
        return std::format("colour space {}", static_cast<int>(space));
    }
}

/// Whether the output runs HDR: a PQ (SMPTE ST 2084) transfer function.
[[nodiscard]] constexpr bool isHdrColorSpace(DXGI_COLOR_SPACE_TYPE space) noexcept
{
    return space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || space == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020;
}

/// A FILETIME's UTC date, "2024-09-05". Driver dates are midnight UTC.
[[nodiscard]] inline std::string fileTimeDate(const FILETIME& time)
{
    const std::uint64_t ticks = Windows::fileTimeTicks(time);
    constexpr std::uint64_t TICKS_PER_DAY = 864'000'000'000ULL; // 100 ns ticks
    const std::chrono::sys_days epoch{std::chrono::year{1601} / 1 / 1};
    const std::chrono::year_month_day date{epoch + std::chrono::days{static_cast<std::int64_t>(ticks / TICKS_PER_DAY)}};
    return std::format("{:%F}", date);
}

[[nodiscard]] inline Monitor readMonitor(const OutputRecord& output, const Functions& fns)
{
    Monitor monitor;
    monitor.hasDesktopRect = true;
    monitor.desktopX = output.desktop.left;
    monitor.desktopY = output.desktop.top;
    monitor.desktopWidth = output.desktop.right - output.desktop.left;
    monitor.desktopHeight = output.desktop.bottom - output.desktop.top;
    if (output.hasDesc1)
    {
        monitor.colorSpace = colorSpaceName(output.colorSpace);
        monitor.hdr = isHdrColorSpace(output.colorSpace);
        monitor.bitsPerColor = output.bitsPerColor;
    }
    const std::vector<std::uint8_t> edid = fns.readMonitorEdid(output.deviceName);
    if (const auto parsed = Edid::parseEdid(edid))
    {
        monitor.name = parsed->displayName();
        monitor.serial = parsed->serialText();
        monitor.widthMm = parsed->widthMm;
        monitor.heightMm = parsed->heightMm;
    }
    return monitor;
}

/// The adapters and monitors, through @p fns.
inline void readGraphics(GraphicsInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;
    const std::optional<Vulkan::Info> vulkan = fns.readVulkan();
    info.vulkanRead = true;
    info.vulkanVersion = vulkan.has_value() ? Vulkan::apiVersionText(vulkan->instanceVersion) : std::string{};
    const std::optional<std::vector<AdapterRecord>> adapters = fns.listAdapters();
    if (!adapters.has_value())
    {
        return;
    }
    info.adaptersRead = true;
    info.monitorsRead = true;
    const std::vector<DriverNode> nodes = fns.listDisplayDrivers();
    std::vector<std::wstring> outputsSeen;
    for (const AdapterRecord& record : *adapters)
    {
        for (const OutputRecord& output : record.outputs)
        {
            if (std::ranges::find(outputsSeen, output.deviceName) == outputsSeen.end())
            {
                outputsSeen.push_back(output.deviceName);
                info.monitors.push_back(readMonitor(output, fns));
            }
        }

        const DXGI_ADAPTER_DESC1& desc = record.desc;
        const bool software = (desc.Flags & static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE)) != 0U;
        if (!shouldListAdapter(software, software ? std::nullopt : fns.adapterType(desc.AdapterLuid)))
        {
            continue;
        }
        GraphicsAdapter adapter;
        adapter.name = WinString::wideToUtf8(static_cast<const wchar_t*>(desc.Description));
        adapter.vendorId = desc.VendorId;
        adapter.deviceId = desc.DeviceId;
        adapter.dedicatedBytes = desc.DedicatedVideoMemory;
        adapter.sharedBytes = desc.SharedSystemMemory;
        const std::optional<PciLocation> location = fns.pciLocation(desc.AdapterLuid);
        if (location.has_value())
        {
            adapter.location = std::format("{:02x}:{:02x}.{}", location->bus, location->device, location->function.value_or(0));
        }
        if (const DriverNode* node = findDriverNode(nodes, desc.VendorId, desc.DeviceId, location))
        {
            adapter.driverVersion = node->driverVersion;
            adapter.driverDate = node->driverDate.has_value() ? fileTimeDate(*node->driverDate) : std::string{};
        }
        if (const std::optional<D3D_FEATURE_LEVEL> level = fns.maxFeatureLevel(desc.AdapterLuid))
        {
            adapter.d3dFeatureLevel = featureLevelName(*level);
        }
        const Vulkan::Device* device =
            vulkan.has_value() ? findVulkanDevice(vulkan->devices, desc.AdapterLuid, desc.VendorId, desc.DeviceId) : nullptr;
        if (device != nullptr)
        {
            adapter.vulkanApiVersion = Vulkan::apiVersionText(device->apiVersion);
            adapter.vulkanDriverVersion = Vulkan::driverVersionText(device->vendorId, device->driverId, device->driverVersion, true);
        }
        info.adapters.push_back(std::move(adapter));
    }
}

} // namespace Platform::WindowsGraphics
