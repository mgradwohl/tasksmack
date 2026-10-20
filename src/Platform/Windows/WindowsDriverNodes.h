#pragma once

// A PnP device node's driver facts, and matching an adapter to its node by PCI location or vendor and
// device ids: what the Graphics & displays section (#1519) and the GPU probe's driver version (#1799)
// both read, and listDriverNodes(), the one SetupAPI lister both read their node lists with (#2565).

#include "Platform/GPUTypes.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <devpropdef.h>
// clang-format on

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::WindowsGraphics
{

// DEVPKEY_Device_DriverDate and _DriverVersion (devpkey.h) and GUID_DEVCLASS_DISPLAY (devguid.h), spelled
// out so no translation unit needs <initguid.h>.
inline constexpr GUID DRIVER_PROPERTY_GUID{
    .Data1 = 0xa8b865dd,
    .Data2 = 0x2e3d,
    .Data3 = 0x4094,
    .Data4 = {0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6},
};
inline constexpr DEVPROPKEY DRIVER_DATE_KEY{.fmtid = DRIVER_PROPERTY_GUID, .pid = 2};
inline constexpr DEVPROPKEY DRIVER_VERSION_KEY{.fmtid = DRIVER_PROPERTY_GUID, .pid = 3};
inline constexpr GUID DISPLAY_CLASS_GUID{
    .Data1 = 0x4d36e968,
    .Data2 = 0xe325,
    .Data3 = 0x11ce,
    .Data4 = {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18},
};

/// The wide characters of the buffer a node's hardware id and driver version are read into; the last
/// one stays 0, so a longer text is not read at all.
inline constexpr std::size_t DRIVER_NODE_TEXT_CHARS = 512;

/// One device node's driver facts.
struct DriverNode
{
    std::string hardwareId;       ///< The first hardware id, "PCI\VEN_10DE&DEV_2786&SUBSYS_..."
    std::optional<DWORD> bus;     ///< SPDRP_BUSNUMBER
    std::optional<DWORD> address; ///< SPDRP_ADDRESS: (device << 16) | function for PCI
    std::string driverVersion;    ///< DEVPKEY_Device_DriverVersion
    std::optional<FILETIME> driverDate;
};

/// The SetupAPI calls listDriverNodes() makes: the system's by default, a test's fakes
/// (test_WindowsDriverNodes.cpp).
struct SetupApiFunctions
{
    using GetClassDevsFn = decltype(&::SetupDiGetClassDevsW);
    using DestroyDeviceInfoListFn = decltype(&::SetupDiDestroyDeviceInfoList);
    using EnumDeviceInfoFn = decltype(&::SetupDiEnumDeviceInfo);
    using GetDeviceRegistryPropertyFn = decltype(&::SetupDiGetDeviceRegistryPropertyW);
    using GetDevicePropertyFn = decltype(&::SetupDiGetDevicePropertyW);

    GetClassDevsFn getClassDevs = &::SetupDiGetClassDevsW;
    DestroyDeviceInfoListFn destroyDeviceInfoList = &::SetupDiDestroyDeviceInfoList;
    EnumDeviceInfoFn enumDeviceInfo = &::SetupDiEnumDeviceInfo;
    GetDeviceRegistryPropertyFn getRegistryProperty = &::SetupDiGetDeviceRegistryPropertyW;
    GetDevicePropertyFn getProperty = &::SetupDiGetDevicePropertyW;
};

/// Every device node SetupDiGetClassDevsW(@p classGuid, @p enumerator, nullptr, @p flags) lists, with its
/// first hardware id, bus number and address, and driver version and date (#2565): the GPU probe reads
/// every present PCI node in any class (#1799), System Information the present Display-class ones
/// (#1519). Read from the PnP manager's records, so no device is woken. A property a node doesn't
/// report, or reports with another type, leaves its field empty; no nodes when the list can't be made.
[[nodiscard]] std::vector<DriverNode>
listDriverNodes(const GUID* classGuid, const wchar_t* enumerator, DWORD flags, const SetupApiFunctions& api = {});

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
