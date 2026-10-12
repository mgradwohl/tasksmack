#pragma once

// A PnP device node's driver facts, and matching an adapter to its node by PCI location or vendor and
// device ids: what the Graphics & displays section (#1519) and the GPU probe's driver version (#1799)
// both read. The node lists themselves are read through SetupAPI by their callers.

#include "Platform/GPUTypes.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::WindowsGraphics
{

/// One device node's driver facts.
struct DriverNode
{
    std::string hardwareId;       ///< The first hardware id, "PCI\VEN_10DE&DEV_2786&SUBSYS_..."
    std::optional<DWORD> bus;     ///< SPDRP_BUSNUMBER
    std::optional<DWORD> address; ///< SPDRP_ADDRESS: (device << 16) | function for PCI
    std::string driverVersion;    ///< DEVPKEY_Device_DriverVersion
    std::optional<FILETIME> driverDate;
};

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

/// Give each adapter in @p adapters the driver version of its PnP device node in @p nodes (#1799):
/// matched by PCI location, or by vendor and device ids where the location is unknown (see
/// findDriverNode()). DXGI has no driver version, and DXCore reads 0.0.0.0 for an NPU, which has no
/// user-mode driver, so this is where every adapter's comes from, a GPU's and an NPU's alike. An
/// adapter with no matching node, or one whose node has no version, keeps the one it has.
inline void applyDriverVersions(std::vector<GPUInfo>& adapters, std::span<const DriverNode> nodes)
{
    for (GPUInfo& adapter : adapters)
    {
        const std::uint32_t vendorId = adapter.pciDeviceId & 0xFFFFU;
        const std::uint32_t deviceId = adapter.pciDeviceId >> 16U;
        const DriverNode* node = findDriverNode(nodes, vendorId, deviceId, adapter.pciLocation);
        if (node != nullptr && !node->driverVersion.empty())
        {
            adapter.driverVersion = node->driverVersion;
        }
    }
}

} // namespace Platform::WindowsGraphics
