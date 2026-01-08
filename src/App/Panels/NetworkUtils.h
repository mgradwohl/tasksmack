#pragma once

#include "Domain/Numeric.h"
#include "Domain/SystemSnapshot.h"

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace App::NetworkUtils
{

/// Crop the front of a vector to reach a target size.
/// Used to align history buffers when they have different lengths.
template<typename T> void cropFrontToSize(std::vector<T>& data, size_t targetSize)
{
    if (data.size() > targetSize)
    {
        const size_t removeCount = data.size() - targetSize;
        using Diff = std::ptrdiff_t;
        const Diff removeCountDiff = Domain::Numeric::narrowOr<Diff>(removeCount, std::numeric_limits<Diff>::min());
        if (removeCountDiff == std::numeric_limits<Diff>::min())
        {
            data.clear();
            return;
        }

        data.erase(data.begin(), data.begin() + removeCountDiff);
    }
}

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
        return "\xef\x8a\x93"; // ICON_FA_BLUETOOTH
    }

    // WiFi detection
    if (name.starts_with("wl") || name.contains("Wi-Fi") || name.contains("WiFi") || name.contains("Wireless") ||
        displayName.contains("Wi-Fi") || displayName.contains("WiFi") || displayName.contains("Wireless"))
    {
        return "\xef\x87\xab"; // ICON_FA_WIFI
    }

    // Virtual/cloud interfaces
    if (isVirtualInterface(iface))
    {
        return "\xef\x83\x82"; // ICON_FA_CLOUD
    }

    // Loopback/localhost
    if (name == "lo" || name.contains("Loopback"))
    {
        return "\xef\x80\x95"; // ICON_FA_HOUSE
    }

    // Default to ethernet
    return "\xef\x9e\x96"; // ICON_FA_ETHERNET
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

                          // 3. Higher link speed first (0 = unknown, sort last)
                          if (a.linkSpeedMbps != b.linkSpeedMbps)
                          {
                              if (a.linkSpeedMbps > 0 && b.linkSpeedMbps > 0)
                              {
                                  return a.linkSpeedMbps > b.linkSpeedMbps;
                              }
                              return a.linkSpeedMbps > b.linkSpeedMbps;
                          }

                          // 4. Alphabetically by display name
                          const auto& nameA = a.displayName.empty() ? a.name : a.displayName;
                          const auto& nameB = b.displayName.empty() ? b.name : b.displayName;
                          return nameA < nameB;
                      });

    return result;
}

} // namespace App::NetworkUtils
