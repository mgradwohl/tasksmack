#pragma once

#include "Domain/SystemSnapshot.h"
#include "UI/IconsFontAwesome6.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App::NetInterfaceUtils
{

/// Check if an interface is likely a virtual/loopback interface that users rarely care about
[[nodiscard]] inline bool isVirtualInterface(const Domain::SystemSnapshot::InterfaceSnapshot& iface)
{
    const auto& name = iface.name;

    // Common loopback names
    if (name == "lo" || name == "Loopback Pseudo-Interface 1" || name.contains("loopback") || name.contains("Loopback"))
    {
        return true;
    }

    // Docker/container interfaces (Linux)
    if (name.starts_with("docker") || name.starts_with("veth") || name.starts_with("br-"))
    {
        return true;
    }

    // VPN/tunnel interfaces (Linux)
    if (name.starts_with("tun") || name.starts_with("tap"))
    {
        return true;
    }

    // WSL interfaces (Windows)
    if (name.contains("WSL") || name.contains("vEthernet"))
    {
        return true;
    }

    // Windows virtual adapters - WAN Miniport, Microsoft virtual adapters
    if (name.starts_with("WAN Miniport") || name.starts_with("Microsoft"))
    {
        return true;
    }

    // Windows filter drivers and packet schedulers (usually duplicates of real adapters)
    if (name.contains("QoS Packet Scheduler") || name.contains("WFP") || name.contains("LightWeight Filter") ||
        name.contains("Native WiFi Filter") || name.contains("Native MAC Layer"))
    {
        return true;
    }

    // 6to4 tunnel adapter
    if (name.contains("6to4"))
    {
        return true;
    }

    // Teredo tunneling
    if (name.contains("Teredo"))
    {
        return true;
    }

    // IP-HTTPS
    if (name.contains("IP-HTTPS"))
    {
        return true;
    }

    // Kernel Debug
    if (name.contains("Kernel Debug"))
    {
        return true;
    }

    // Wi-Fi Direct virtual adapters
    if (name.contains("Wi-Fi Direct"))
    {
        return true;
    }

    return false;
}

/// Check if an interface is Bluetooth (usually not useful for throughput monitoring)
[[nodiscard]] inline bool isBluetoothInterface(const Domain::SystemSnapshot::InterfaceSnapshot& iface)
{
    const auto& name = iface.name;
    const auto& displayName = iface.displayName;

    return name.contains("Bluetooth") || displayName.contains("Bluetooth") || name.contains("bluetooth") || name.contains("bnep");
}

/// Determine the interface type icon based on name patterns
[[nodiscard]] inline const char* getInterfaceTypeIcon(const Domain::SystemSnapshot::InterfaceSnapshot& iface)
{
    const auto& name = iface.name;
    const auto& displayName = iface.displayName;

    // Check for Bluetooth first
    if (isBluetoothInterface(iface))
    {
        return ICON_FA_BLUETOOTH;
    }

    // WiFi detection
    if (name.starts_with("wl") || name.contains("Wi-Fi") || name.contains("WiFi") || name.contains("Wireless") ||
        displayName.contains("Wi-Fi") || displayName.contains("WiFi") || displayName.contains("Wireless"))
    {
        return ICON_FA_WIFI;
    }

    // Virtual/cloud interfaces
    if (isVirtualInterface(iface))
    {
        return ICON_FA_CLOUD;
    }

    // Loopback/localhost
    if (name == "lo" || name.contains("Loopback"))
    {
        return ICON_FA_HOUSE;
    }

    // Default to ethernet
    return ICON_FA_ETHERNET;
}

/// Sort interfaces: Up first, then by activity, then by speed, then alphabetically
[[nodiscard]] inline std::vector<Domain::SystemSnapshot::InterfaceSnapshot>
getSortedFilteredInterfaces(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces,
                            bool showVirtualInterfaces = false,
                            bool showDownInterfaces = true)
{
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> result;
    result.reserve(interfaces.size());

    // Apply filters
    for (const auto& iface : interfaces)
    {
        // Skip virtual/bluetooth if not showing all
        if (!showVirtualInterfaces && (isVirtualInterface(iface) || isBluetoothInterface(iface)))
        {
            continue;
        }

        // Skip down interfaces if not showing them
        if (!showDownInterfaces && !iface.isUp)
        {
            continue;
        }

        result.push_back(iface);
    }

    // Sort: Up first, then by activity, then by speed, then alphabetically
    std::ranges::sort(result,
                      [](const auto& a, const auto& b)
                      {
                          // 1. Up interfaces first
                          if (a.isUp != b.isUp)
                          {
                              return a.isUp > b.isUp;
                          }

                          // 2. Interfaces with activity first
                          const bool hasActivityA = (a.txBytesPerSec + a.rxBytesPerSec) > 0.0;
                          const bool hasActivityB = (b.txBytesPerSec + b.rxBytesPerSec) > 0.0;
                          if (hasActivityA != hasActivityB)
                          {
                              return hasActivityA > hasActivityB;
                          }

                          // 3. Higher link speed first (0 = unknown, sort last among interfaces with same status)
                          if (a.linkSpeedMbps != b.linkSpeedMbps)
                          {
                              return a.linkSpeedMbps > b.linkSpeedMbps;
                          }

                          // 4. Alphabetically by display name
                          const auto& nameA = a.displayName.empty() ? a.name : a.displayName;
                          const auto& nameB = b.displayName.empty() ? b.name : b.displayName;
                          return nameA < nameB;
                      });

    return result;
}

/// Names of the interfaces seen sending or receiving this session (heterogeneous lookup by string_view).
using InterfaceNameSet = std::set<std::string, std::less<>>;

/// Whether the interface is moving any traffic in this snapshot.
[[nodiscard]] inline bool hasTraffic(const Domain::SystemSnapshot::InterfaceSnapshot& iface)
{
    return iface.rxBytesPerSec > 0.0 || iface.txBytesPerSec > 0.0;
}

/// Whether the interface is hidden by default whatever its traffic: virtual and Bluetooth
/// interfaces (the platform's isVirtual flag where it is set, the name heuristic otherwise) (#1211).
[[nodiscard]] inline bool isAlwaysHidden(const Domain::SystemSnapshot::InterfaceSnapshot& iface)
{
    return iface.isVirtual || isVirtualInterface(iface) || isBluetoothInterface(iface);
}

/// Add every interface moving traffic in this snapshot to `seen`, so a down interface that carried
/// traffic earlier in the session (an unplugged USB adapter, a dropped VPN) stays listed (#1211).
/// Always-hidden interfaces are not recorded: their membership changes nothing, and on a container
/// host their short-lived veth/bridge names would otherwise accumulate for the session.
inline void recordInterfaceTraffic(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces, InterfaceNameSet& seen)
{
    for (const auto& iface : interfaces)
    {
        if (hasTraffic(iface) && !isAlwaysHidden(iface) && !seen.contains(iface.name))
        {
            seen.insert(iface.name);
        }
    }
}

/// Whether the Interface Status table leaves the interface out unless "Show all" is on (#1211).
///
/// Virtual and Bluetooth interfaces are hidden (the platform's isVirtual flag where it is set, the
/// name heuristic otherwise), as are down interfaces -- WAN Miniports, spare Wi-Fi instances,
/// disconnected adapters -- unless they have carried traffic this session.
[[nodiscard]] inline bool isHiddenByDefault(const Domain::SystemSnapshot::InterfaceSnapshot& iface, const InterfaceNameSet& seenTraffic)
{
    if (isAlwaysHidden(iface))
    {
        return true;
    }
    return !iface.isUp && !hasTraffic(iface) && !seenTraffic.contains(iface.name);
}

/// How many interfaces the Interface Status table hides by default; the "Show all (N)" count (#1211).
[[nodiscard]] inline std::size_t countHiddenInterfaces(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces,
                                                       const InterfaceNameSet& seenTraffic)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(interfaces, [&seenTraffic](const auto& iface) { return isHiddenByDefault(iface, seenTraffic); }));
}

/// The Interface Status table's rows: every interface when `showAll`, otherwise those not hidden by
/// default (see isHiddenByDefault), sorted as getSortedFilteredInterfaces sorts them (#1211).
[[nodiscard]] inline std::vector<Domain::SystemSnapshot::InterfaceSnapshot> getInterfaceStatusRows(
    const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces, bool showAll, const InterfaceNameSet& seenTraffic)
{
    if (showAll)
    {
        return getSortedFilteredInterfaces(interfaces, /*showVirtualInterfaces=*/true, /*showDownInterfaces=*/true);
    }
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> shown;
    shown.reserve(interfaces.size());
    std::ranges::copy_if(
        interfaces, std::back_inserter(shown), [&seenTraffic](const auto& iface) { return !isHiddenByDefault(iface, seenTraffic); });
    return getSortedFilteredInterfaces(shown, /*showVirtualInterfaces=*/true, /*showDownInterfaces=*/true);
}

/// The network chart's interface selector entries: the Total first, then each interface's display
/// name (or system name) in list order.
///
/// The platform marks software interfaces (bridges, veth, VPN tunnels) virtual, and the Total leaves
/// them out because their traffic also crosses a hardware interface -- unless no hardware interface
/// is listed at all, as SystemModel's Total does (#1106). Those interfaces stay selectable; their
/// entries say they aren't in the Total, and the Total's entry says what it sums.
[[nodiscard]] inline std::vector<std::string>
interfaceSelectorLabels(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces)
{
    const bool anyHardware = std::ranges::any_of(interfaces, [](const auto& iface) { return !iface.isVirtual; });
    const bool anyExcluded = anyHardware && std::ranges::any_of(interfaces, [](const auto& iface) { return iface.isVirtual; });

    std::vector<std::string> labels;
    labels.reserve(interfaces.size() + 1);
    labels.emplace_back(anyExcluded ? "Total (Hardware Interfaces)" : "Total (All Interfaces)");
    for (const auto& iface : interfaces)
    {
        std::string label = iface.displayName.empty() ? iface.name : iface.displayName;
        if (anyHardware && iface.isVirtual)
        {
            label += " (virtual, not in Total)";
        }
        labels.push_back(std::move(label));
    }
    return labels;
}

/// Where the network chart's selected interface is in this frame's interface list.
struct InterfaceSelection
{
    std::optional<std::size_t> index; ///< Position in the list; nullopt shows "Total".
    bool lost = false;                ///< A named interface was selected but is no longer listed.
};

/// Resolve a selection held by interface name (empty = "Total") against this frame's list.
///
/// The selection is a name, not a position: a position silently switched the chart to a different
/// interface whenever the list was reordered or an interface was added or removed (#996). When the
/// named interface is gone, `lost` tells the caller to fall back to Total and restart the bars.
[[nodiscard]] inline InterfaceSelection resolveInterfaceSelection(const std::vector<Domain::SystemSnapshot::InterfaceSnapshot>& interfaces,
                                                                  std::string_view selectedName)
{
    if (selectedName.empty())
    {
        return {};
    }
    const auto it = std::ranges::find_if(interfaces, [selectedName](const auto& iface) { return iface.name == selectedName; });
    if (it == interfaces.end())
    {
        return {.index = std::nullopt, .lost = true};
    }
    return {.index = static_cast<std::size_t>(std::distance(interfaces.begin(), it)), .lost = false};
}

} // namespace App::NetInterfaceUtils
