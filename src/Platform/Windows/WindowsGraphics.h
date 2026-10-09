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
// Every call goes through an injectable table; WindowsSystemInfoProbe.cpp supplies the real one, tests
// substitute fakes.

#include "Platform/EdidParser.h"
#include "Platform/GPUTypes.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/DXGIGPUProbeMath.h"
#include "Platform/Windows/WinString.h"

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
#pragma clang diagnostic pop
// clang-format on

#include <algorithm>
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

/// One display-class device node's driver facts.
struct DriverNode
{
    std::string hardwareId;       ///< The first hardware id, "PCI\VEN_10DE&DEV_2786&SUBSYS_..."
    std::optional<DWORD> bus;     ///< SPDRP_BUSNUMBER
    std::optional<DWORD> address; ///< SPDRP_ADDRESS: (device << 16) | function for PCI
    std::string driverVersion;
    std::optional<FILETIME> driverDate;
};

/// The calls readGraphics() makes.
struct Functions
{
    std::optional<std::vector<AdapterRecord>> (*listAdapters)() = nullptr; ///< nullopt when DXGI can't be used
    std::optional<AdapterTypeBits> (*adapterType)(const LUID& luid) = nullptr;
    std::optional<PciLocation> (*pciLocation)(const LUID& luid) = nullptr;
    std::vector<DriverNode> (*listDisplayDrivers)() = nullptr;
    std::vector<std::uint8_t> (*readMonitorEdid)(const std::wstring& outputDeviceName) = nullptr; ///< Empty when unreadable
};

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
    const std::uint64_t ticks = (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
    constexpr std::uint64_t TICKS_PER_DAY = 864'000'000'000ULL; // 100 ns ticks
    const std::chrono::sys_days epoch{std::chrono::year{1601} / 1 / 1};
    const std::chrono::year_month_day date{epoch + std::chrono::days{static_cast<std::int64_t>(ticks / TICKS_PER_DAY)}};
    return std::format("{:%F}", date);
}

/// The device node of the adapter with @p vendorId and @p deviceId: the one at @p location when it is
/// known and a node reports it, else the first with those ids. Null when none matches.
[[nodiscard]] inline const DriverNode* findDriverNode(std::span<const DriverNode> nodes,
                                                      std::uint32_t vendorId,
                                                      std::uint32_t deviceId,
                                                      const std::optional<PciLocation>& location)
{
    const std::string ids = std::format("VEN_{:04X}&DEV_{:04X}", vendorId, deviceId);
    const DriverNode* first = nullptr;
    for (const DriverNode& node : nodes)
    {
        std::string upper = node.hardwareId;
        std::ranges::transform(upper, upper.begin(), [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
        if (!upper.contains(ids))
        {
            continue;
        }
        if (location.has_value() && node.bus.has_value() && node.address.has_value())
        {
            const DWORD address = (location->device << 16U) | location->function.value_or(0);
            if (*node.bus == location->bus && *node.address == address)
            {
                return &node;
            }
            continue;
        }
        first = first != nullptr ? first : &node;
    }
    return first;
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
        info.adapters.push_back(std::move(adapter));
    }
}

} // namespace Platform::WindowsGraphics
