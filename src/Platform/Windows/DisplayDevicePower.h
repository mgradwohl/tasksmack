#pragma once

#include "Platform/GPUTypes.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
// clang-format on

namespace Platform
{

/// Whether a device power state (CM_POWER_DATA::PD_MostRecentPowerState, a DEVICE_POWER_STATE)
/// means the device is asleep: D1-D3, as a hybrid laptop's runtime-suspended dGPU is. D0 and an
/// unspecified state are awake (#1265).
[[nodiscard]] constexpr bool isAsleepDevicePowerState(std::uint32_t state) noexcept
{
    return state >= static_cast<std::uint32_t>(PowerDeviceD1) && state <= static_cast<std::uint32_t>(PowerDeviceD3);
}

/// A PCI device's bus location from its devnode's DEVPKEY_Device_BusNumber and
/// DEVPKEY_Device_Address; for PCI the address is (device << 16) | function (#1265).
[[nodiscard]] constexpr PciLocation pciLocationFromDevNode(std::uint32_t busNumber, std::uint32_t address) noexcept
{
    return PciLocation{.bus = busNumber, .device = address >> 16U, .function = address & 0xFFFFU};
}

/// Whether a display adapter is asleep (in a low device power state), read from the PnP manager's
/// record of the device's power state rather than from the GPU, so asking never wakes it. This is
/// Windows' counterpart of Linux's PCI power/runtime_status: NVML queries can wake a hybrid
/// laptop's runtime-suspended (RTD3/D3cold) NVIDIA dGPU, so the NVML probe asks this first and
/// leaves a sleeping GPU alone (#1265). The display-class devnode for each PCI location is looked
/// up once and cached; anything that can't be read counts as awake, so a GPU is never left
/// unmonitored by mistake.
class DisplayDevicePower
{
  public:
    /// Whether the display adapter at @p location is asleep now. False when unknown.
    [[nodiscard]] bool isAsleep(const PciLocation& location)
    {
        const std::optional<DEVINST> devNode = devNodeFor(location);
        if (!devNode.has_value())
        {
            return false;
        }
        std::array<BYTE, sizeof(CM_POWER_DATA)> buffer{};
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        auto size = static_cast<ULONG>(buffer.size());
        const CONFIGRET result = CM_Get_DevNode_PropertyW(*devNode, &POWER_DATA_KEY, &type, buffer.data(), &size, 0);
        if (result != CR_SUCCESS || type != DEVPROP_TYPE_BINARY || size < offsetof(CM_POWER_DATA, PD_Capabilities))
        {
            if (result == CR_NO_SUCH_DEVINST)
            {
                m_DevNodes.erase(locationKey(location)); // Removed or re-created: look it up again next time
            }
            return false;
        }
        CM_POWER_DATA power{};
        std::memcpy(&power, buffer.data(), std::min<std::size_t>(size, sizeof(power)));
        return isAsleepDevicePowerState(static_cast<std::uint32_t>(power.PD_MostRecentPowerState));
    }

    /// Forget the cached devnodes, so the next isAsleep() looks each adapter up again (after a
    /// re-enumeration, when devices may have come or gone).
    void reset()
    {
        m_DevNodes.clear();
    }

  private:
    // DEVPKEY_Device_BusNumber/Address/PowerData (devpkey.h), defined here rather than through
    // initguid.h so including this header never turns other GUID declarations into definitions.
    static constexpr GUID DEVICE_PROPERTY_GUID{
        .Data1 = 0xa45c254e, .Data2 = 0xdf1c, .Data3 = 0x4efd, .Data4 = {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}};
    static constexpr DEVPROPKEY BUS_NUMBER_KEY{.fmtid = DEVICE_PROPERTY_GUID, .pid = 23};
    static constexpr DEVPROPKEY ADDRESS_KEY{.fmtid = DEVICE_PROPERTY_GUID, .pid = 30};
    static constexpr DEVPROPKEY POWER_DATA_KEY{.fmtid = DEVICE_PROPERTY_GUID, .pid = 32};

    [[nodiscard]] static std::uint64_t locationKey(const PciLocation& location) noexcept
    {
        // The function in the low 16 bits (all ones when unknown), the device above it.
        const std::uint64_t function = location.function.value_or(0xFFFFU) & 0xFFFFU;
        return (static_cast<std::uint64_t>(location.bus) << 32U) | (static_cast<std::uint64_t>(location.device & 0xFFFFU) << 16U) |
               function;
    }

    [[nodiscard]] static std::optional<std::uint32_t> readUInt32(DEVINST devNode, const DEVPROPKEY& key)
    {
        std::array<BYTE, sizeof(std::uint32_t)> buffer{};
        DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
        auto size = static_cast<ULONG>(buffer.size());
        if (CM_Get_DevNode_PropertyW(devNode, &key, &type, buffer.data(), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_UINT32 ||
            size != buffer.size())
        {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        std::memcpy(&value, buffer.data(), sizeof(value));
        return value;
    }

    /// The present display-class devnode at @p location, from the cache or by searching once.
    [[nodiscard]] std::optional<DEVINST> devNodeFor(const PciLocation& location)
    {
        const std::uint64_t key = locationKey(location);
        if (const auto it = m_DevNodes.find(key); it != m_DevNodes.end())
        {
            return it->second;
        }
        const std::optional<DEVINST> found = findDevNode(location);
        m_DevNodes.emplace(key, found);
        return found;
    }

    [[nodiscard]] static std::optional<DEVINST> findDevNode(const PciLocation& location)
    {
        // GUID_DEVCLASS_DISPLAY
        const std::wstring displayClass = L"{4d36e968-e325-11ce-bfc1-08002be10318}";
        constexpr ULONG FLAGS = CM_GETIDLIST_FILTER_CLASS | CM_GETIDLIST_FILTER_PRESENT;
        ULONG length = 0;
        if (CM_Get_Device_ID_List_SizeW(&length, displayClass.c_str(), FLAGS) != CR_SUCCESS || length == 0)
        {
            return std::nullopt;
        }
        std::wstring ids(length, L'\0');
        if (CM_Get_Device_ID_ListW(displayClass.c_str(), ids.data(), length, FLAGS) != CR_SUCCESS)
        {
            return std::nullopt;
        }
        // A list of NUL-terminated device instance ids, ended by an empty one.
        std::size_t start = 0;
        while (start < ids.size() && ids[start] != L'\0')
        {
            const std::size_t end = ids.find(L'\0', start);
            std::wstring id = ids.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            start = (end == std::wstring::npos) ? ids.size() : end + 1;

            DEVINST devNode = 0;
            if (CM_Locate_DevNodeW(&devNode, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            {
                continue;
            }
            const auto bus = readUInt32(devNode, BUS_NUMBER_KEY);
            const auto address = readUInt32(devNode, ADDRESS_KEY);
            if (bus.has_value() && address.has_value() && samePciLocation(pciLocationFromDevNode(*bus, *address), location))
            {
                return devNode;
            }
        }
        return std::nullopt;
    }

    std::unordered_map<std::uint64_t, std::optional<DEVINST>> m_DevNodes;
};

} // namespace Platform
