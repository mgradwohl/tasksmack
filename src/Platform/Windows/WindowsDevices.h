#pragma once

// The Windows Devices facts (#1520), all unprivileged and enumeration only:
// - Every present device node from SetupAPI (DIGCF_ALLCLASSES | DIGCF_PRESENT): its instance id and
//   parent's, name, manufacturer, setup class description, service and PCI bus and address, and its
//   status and problem code from CM_Get_DevNode_Status. A node with DN_HAS_PROBLEM is a problem device,
//   with Device Manager's message for its code.
// - PCI devices are the nodes the PCI bus enumerated ("PCI\..."); USB devices those the USB hub driver
//   did ("USB\VID_..."), less root hubs and the interfaces of composite devices ("&MI_").
// - A USB device's link speed (#1642), from the hub it is plugged into: its port (the "Port_#NNNN" of
//   its location information, else its address) asked of the hub with the query-only connection
//   information IOCTLs, one hub open per hub per read.
// - Active audio endpoints from the MMDevice API, with their data flow.
// Every call goes through an injectable table; WindowsSystemInfoProbe.cpp supplies the real one, tests
// substitute fakes.

#include "Platform/ISystemInfoProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <usbspec.h>
// clang-format on

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::WindowsDevices
{

/// One present device node, as listDevNodes() reads it.
struct DevNodeRecord
{
    std::string instanceId;       ///< "PCI\VEN_10DE&DEV_2786&SUBSYS_...\4&2C3D...&0&0008", upper case
    std::string parentId;         ///< The parent node's instance id; empty for the root
    std::string name;             ///< SPDRP_FRIENDLYNAME, else SPDRP_DEVICEDESC
    std::string manufacturer;     ///< SPDRP_MFG
    std::string className;        ///< The setup class's description ("Display adapters")
    std::string service;          ///< SPDRP_SERVICE: the driver's service name
    std::optional<DWORD> bus;     ///< SPDRP_BUSNUMBER
    std::optional<DWORD> address; ///< SPDRP_ADDRESS: (device << 16) | function for PCI; the hub port for USB
    std::string locationInfo;     ///< SPDRP_LOCATION_INFORMATION: "Port_#0003.Hub_#0001" for a USB device
    bool statusRead = false;      ///< CM_Get_DevNode_Status answered: status and problem are set
    ULONG status = 0;             ///< DN_* flags
    ULONG problem = 0;            ///< CM_PROB_*
};

/// One active audio endpoint.
struct AudioRecord
{
    std::string name;     ///< PKEY_Device_FriendlyName
    bool capture = false; ///< eCapture (an input); eRender otherwise
};

/// What a hub says about one of its ports, as readHubPorts() reads it; a part it couldn't read is unset.
struct UsbPortRecord
{
    std::optional<UCHAR> speed;                   ///< ..._INFORMATION_EX: the USB_DEVICE_SPEED of a connected device
    bool superSpeed = false;                      ///< ..._EX_V2: DeviceIsOperatingAtSuperSpeedOrHigher
    bool superSpeedPlus = false;                  ///< ..._EX_V2: DeviceIsOperatingAtSuperSpeedPlusOrHigher
    std::optional<ULONG> superSpeedPlusLaneSpeed; ///< ..._SUPERSPEEDPLUS_INFORMATION: RxSuperSpeedPlus, as a ULONG
    ULONG superSpeedPlusLanes = 0;                ///< ..._SUPERSPEEDPLUS_INFORMATION: RxLaneCount (the lanes less one)
};

/// The calls readDevices() makes.
struct Functions
{
    std::optional<std::vector<DevNodeRecord>> (*listDevNodes)() = nullptr;     ///< nullopt when SetupAPI fails
    std::optional<std::vector<AudioRecord>> (*listAudioEndpoints)() = nullptr; ///< nullopt when MMDevice can't be used
    /// The hub's answers for @p ports, by port; a port it couldn't answer for (or every port, when the hub
    /// can't be opened) is left out. Optional: without it no USB speed is read.
    std::map<ULONG, UsbPortRecord> (*readHubPorts)(const std::string& hubInstanceId, const std::vector<ULONG>& ports) = nullptr;
};

/// Device Manager's message for a CM_PROB_* code, shortened, with the code: "This device is disabled
/// (Code 22)"; "Problem code N" for a code the table doesn't have.
[[nodiscard]] inline std::string problemText(ULONG code)
{
    static const std::map<ULONG, std::string_view> MESSAGES{
        {CM_PROB_NOT_CONFIGURED, "This device is not configured correctly"},
        {CM_PROB_OUT_OF_MEMORY, "The driver may be corrupted, or the system is low on resources"},
        {CM_PROB_FAILED_START, "This device cannot start"},
        {CM_PROB_NORMAL_CONFLICT, "This device cannot find enough free resources"},
        {CM_PROB_NEED_RESTART, "This device cannot work properly until you restart your computer"},
        {CM_PROB_REINSTALL, "Reinstall the drivers for this device"},
        {CM_PROB_REGISTRY, "Its configuration information in the registry is incomplete or damaged"},
        {CM_PROB_WILL_BE_REMOVED, "Windows is removing this device"},
        {CM_PROB_DISABLED, "This device is disabled"},
        {CM_PROB_DEVICE_NOT_THERE, "This device is not present, not working, or missing drivers"},
        {CM_PROB_FAILED_INSTALL, "The drivers for this device are not installed"},
        {CM_PROB_HARDWARE_DISABLED, "The device's firmware did not give it the resources it needs"},
        {CM_PROB_FAILED_ADD, "Windows cannot load the drivers required for this device"},
        {CM_PROB_DISABLED_SERVICE, "A driver (service) for this device has been disabled"},
        {CM_PROB_FAILED_DRIVER_ENTRY, "Windows cannot initialize the device driver"},
        {CM_PROB_DRIVER_FAILED_LOAD, "Windows cannot load the device driver; it may be corrupted or missing"},
        {CM_PROB_FAILED_POST_START, "Windows has stopped this device because it has reported problems"},
        {CM_PROB_PHANTOM, "This device is not connected to the computer"},
        {CM_PROB_HELD_FOR_EJECT, "This device is prepared for safe removal but not removed"},
        {CM_PROB_DRIVER_BLOCKED, "The driver was blocked from starting because it is known to have problems"},
        {CM_PROB_UNSIGNED_DRIVER, "Windows cannot verify the digital signature of the drivers"},
    };
    const auto it = MESSAGES.find(code);
    return it != MESSAGES.end() ? std::format("{} (Code {})", it->second, code) : std::format("Problem code {}", code);
}

/// The four hex digits after @p key ("VEN_", "VID_") in an instance id; 0 when absent.
[[nodiscard]] inline std::uint16_t idAfter(std::string_view instanceId, std::string_view key) noexcept
{
    const std::size_t at = instanceId.find(key);
    std::uint16_t id = 0;
    if (at != std::string_view::npos && instanceId.size() >= at + key.size() + 4)
    {
        const char* first = instanceId.data() + at + key.size();
        static_cast<void>(std::from_chars(first, first + 4, id, 16));
    }
    return id;
}

/// The node's facts as a Device; ids from its instance id with @p vendorKey and @p productKey. The
/// manufacturer is kept only for a node without a name: Windows' names already carry the vendor, and
/// its manufacturer field is often a driver family ("Generic USB xHCI Host Controller") or generic
/// ("(Standard system devices)").
[[nodiscard]] inline Device toDevice(const DevNodeRecord& node, std::string_view vendorKey, std::string_view productKey)
{
    Device device;
    device.name = node.name;
    device.vendor = node.name.empty() && !node.manufacturer.starts_with('(') ? node.manufacturer : std::string{};
    device.vendorId = idAfter(node.instanceId, vendorKey);
    device.productId = idAfter(node.instanceId, productKey);
    device.className = node.className;
    device.driver = node.service;
    if (node.bus.has_value() && node.address.has_value() && node.instanceId.starts_with("PCI\\"))
    {
        device.location = std::format("{:02x}:{:02x}.{}", *node.bus, *node.address >> 16U, *node.address & 0xFFFFU);
    }
    if (node.statusRead && (node.status & DN_HAS_PROBLEM) != 0U)
    {
        device.problem = problemText(node.problem);
    }
    return device;
}

/// Whether the node is a USB device the section lists: not a root hub nor a composite device's interface.
[[nodiscard]] inline bool isListedUsbDevice(std::string_view instanceId) noexcept
{
    return instanceId.starts_with("USB\\VID_") && !instanceId.contains("&MI_");
}

/// The port number in a USB device's location information ("Port_#0003.Hub_#0001" is 3); nullopt
/// when it has none.
[[nodiscard]] inline std::optional<ULONG> portFromLocation(std::string_view location) noexcept
{
    constexpr std::string_view KEY = "Port_#";
    const std::size_t at = location.find(KEY);
    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view digits = location.substr(at + KEY.size());
    ULONG port = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    return error == std::errc{} && end != digits.data() && port != 0 ? std::optional<ULONG>(port) : std::nullopt;
}

/// The hub port a USB device is plugged into: from its location information, else its address.
[[nodiscard]] inline std::optional<ULONG> usbPort(const DevNodeRecord& node) noexcept
{
    if (const std::optional<ULONG> port = portFromLocation(node.locationInfo); port.has_value())
    {
        return port;
    }
    return node.address.has_value() && *node.address != 0 ? std::optional<ULONG>(*node.address) : std::nullopt;
}

/// A SuperSpeedPlus link's speed in Mbit/s from USB_NODE_CONNECTION_SUPERSPEEDPLUS_INFORMATION: the lane
/// speed (a USB_DEVICE_CAPABILITY_SUPERSPEEDPLUS_SPEED: mantissa in bits 16-31, exponent in bits 4-5 for
/// b/s, Kb/s, Mb/s, Gb/s) times the lanes; 0 when that isn't a SuperSpeedPlus rate (10 to 40 Gbit/s).
[[nodiscard]] inline double superSpeedPlusMbps(ULONG laneSpeed, ULONG lanesLessOne) noexcept
{
    constexpr std::array<double, 4> SCALE_TO_MBPS{1e-6, 1e-3, 1.0, 1e3};
    const double lane = static_cast<double>(laneSpeed >> 16U) * SCALE_TO_MBPS[(laneSpeed >> 4U) & 0x3U];
    const double mbps = lane * (static_cast<double>(lanesLessOne) + 1.0);
    return mbps >= 10000.0 && mbps <= 40000.0 ? mbps : 0.0;
}

/// A USB device's link speed in Mbit/s from its hub's answers; 0 when unknown. The EX_V2 flags come
/// first: the older EX speed tops out at SuperSpeed.
[[nodiscard]] inline double usbSpeedMbps(const UsbPortRecord& port) noexcept
{
    if (port.superSpeedPlus)
    {
        const double exact =
            port.superSpeedPlusLaneSpeed.has_value() ? superSpeedPlusMbps(*port.superSpeedPlusLaneSpeed, port.superSpeedPlusLanes) : 0.0;
        return exact > 0.0 ? exact : 10000.0;
    }
    if (port.superSpeed)
    {
        return 5000.0;
    }
    if (!port.speed.has_value())
    {
        return 0.0;
    }
    switch (*port.speed)
    {
    case UsbLowSpeed:
        return 1.5;
    case UsbFullSpeed:
        return 12.0;
    case UsbHighSpeed:
        return 480.0;
    case UsbSuperSpeed:
        return 5000.0;
    default:
        return 0.0;
    }
}

/// Each listed USB device's link speed, by instance id, through @p fns: the devices are grouped by hub
/// so each hub is opened once per read. A device whose speed couldn't be read is left out.
[[nodiscard]] inline std::map<std::string, double, std::less<>> usbSpeeds(const std::vector<DevNodeRecord>& nodes, const Functions& fns)
{
    std::map<std::string, double, std::less<>> speeds;
    if (fns.readHubPorts == nullptr)
    {
        return speeds;
    }
    std::map<std::string, std::vector<std::pair<const DevNodeRecord*, ULONG>>, std::less<>> byHub;
    for (const DevNodeRecord& node : nodes)
    {
        const std::optional<ULONG> port = usbPort(node);
        if (port.has_value() && isListedUsbDevice(node.instanceId) && !node.parentId.empty())
        {
            byHub[node.parentId].emplace_back(&node, *port);
        }
    }
    for (const auto& [hub, devices] : byHub)
    {
        std::vector<ULONG> ports;
        ports.reserve(devices.size());
        for (const auto& device : devices)
        {
            ports.push_back(device.second);
        }
        const std::map<ULONG, UsbPortRecord> answers = fns.readHubPorts(hub, ports);
        for (const auto& [node, port] : devices)
        {
            const auto answer = answers.find(port);
            if (const double mbps = answer != answers.end() ? usbSpeedMbps(answer->second) : 0.0; mbps > 0.0)
            {
                speeds.emplace(node->instanceId, mbps);
            }
        }
    }
    return speeds;
}

/// The USB devices in tree order (each hub followed by what hangs off it), with depth and parent; the
/// serial number is the instance id's last part when the device gave one (a generated one has '&'),
/// and the link speed is from @p speedsMbps (by instance id) when it has one.
[[nodiscard]] inline std::vector<Device> usbTree(const std::vector<DevNodeRecord>& nodes,
                                                 const std::map<std::string, double, std::less<>>& speedsMbps = {})
{
    std::set<std::string_view> listed;
    for (const DevNodeRecord& node : nodes)
    {
        if (isListedUsbDevice(node.instanceId))
        {
            listed.insert(node.instanceId);
        }
    }
    // Depth first, in enumeration order: a stack of (node, parent's index, depth), pushed in reverse.
    struct Pending
    {
        const DevNodeRecord* node;
        std::optional<std::size_t> parent;
        std::uint32_t depth;
    };
    std::vector<Pending> stack;
    const auto pushChildren = [&](std::string_view parentId, std::optional<std::size_t> parent, std::uint32_t depth)
    {
        for (const DevNodeRecord& node : std::ranges::reverse_view(nodes))
        {
            const bool child = parent.has_value() ? node.parentId == parentId : !listed.contains(node.parentId);
            if (child && listed.contains(node.instanceId))
            {
                stack.push_back({.node = &node, .parent = parent, .depth = depth});
            }
        }
    };
    pushChildren({}, std::nullopt, 1);
    std::vector<Device> devices;
    while (!stack.empty() && devices.size() < listed.size()) // the bound guards against a parent cycle
    {
        const Pending next = stack.back();
        stack.pop_back();
        Device device = toDevice(*next.node, "VID_", "PID_");
        device.depth = next.depth;
        device.parent = next.parent;
        const std::string_view last = std::string_view(next.node->instanceId).substr(next.node->instanceId.rfind('\\') + 1);
        device.serial = last.contains('&') ? std::string{} : std::string(last);
        if (const auto speed = speedsMbps.find(next.node->instanceId); speed != speedsMbps.end())
        {
            device.speedMbps = speed->second;
        }
        devices.push_back(std::move(device));
        pushChildren(next.node->instanceId, devices.size() - 1, next.depth + 1);
    }
    return devices;
}

/// The devices, problems and audio endpoints, through @p fns.
inline void readDevices(DevicesInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;
    if (const std::optional<std::vector<DevNodeRecord>> nodes = fns.listDevNodes(); nodes.has_value())
    {
        info.pciRead = true;
        info.usbRead = true;
        info.problemsRead = true;
        for (const DevNodeRecord& node : *nodes)
        {
            const bool pci = node.instanceId.starts_with("PCI\\");
            Device device = pci ? toDevice(node, "VEN_", "DEV_") : toDevice(node, "VID_", "PID_");
            if (!device.problem.empty())
            {
                info.problems.push_back(device);
            }
            if (pci)
            {
                info.pci.push_back(std::move(device));
            }
        }
        std::ranges::sort(info.pci, {}, &Device::location);
        info.usb = usbTree(*nodes, usbSpeeds(*nodes, fns));
    }
    if (const std::optional<std::vector<AudioRecord>> endpoints = fns.listAudioEndpoints(); endpoints.has_value())
    {
        info.audioRead = true;
        for (const AudioRecord& endpoint : *endpoints)
        {
            info.audio.push_back({.name = endpoint.name, .flow = endpoint.capture ? AudioFlow::Input : AudioFlow::Output});
        }
    }
}

} // namespace Platform::WindowsDevices
