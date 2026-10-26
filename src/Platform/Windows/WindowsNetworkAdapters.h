#pragma once

// The Windows Network adapters facts (#1518), unprivileged and read only:
//
//   1. GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS): per adapter its connection name and
//      description, state, MTU, MAC, unicast addresses with their prefix lengths, gateways, DNS
//      servers, search domains (the DNS suffix, then the adapter's suffix search list), and whether
//      IPv4 comes from DHCP, from which server and until when. The loopback adapter is left out. The
//      section's DNS servers and search domains are those of the up adapters, without repeats.
//   2. GetIpForwardTable2, once for IPv4 and once for IPv6 (#2996): the section's default gateway is the
//      next hop of the cheapest default route (0.0.0.0/0, ::/0) through an up adapter, ranked as Windows
//      ranks them, by the route's metric plus its interface's metric (the adapter's Ipv4Metric or
//      Ipv6Metric). A table that can't be read leaves the choice to the interface metric alone: the
//      first gateway of the up adapter with the lowest one.
//   3. wlanapi.dll, loaded at run time from System32 (it is missing on Server without the Wireless LAN
//      Service): per Wi-Fi interface its current connection (network name, signal quality, PHY type and
//      link rates), the RSSI, and the channel's centre frequency from the cached BSS list, else the
//      channel number alone. Windows 11 24H2 and later refuse the connection and the BSS list unless
//      location access is on for desktop apps; the RSSI and channel still read then. Without
//      wlanapi.dll, or the WLAN AutoConfig service, the Wi-Fi network row says so.
//
//      The current connection (WlanQueryInterface's wlan_intf_opcode_current_connection) and the BSS
//      list (WlanGetNetworkBssList) are on Microsoft's "Wi-Fi access location changes" list: the first
//      call can raise a one-time location prompt. So they are made only when the read asks for the
//      Wi-Fi details (#1782), which the System tab does once "Show Wi-Fi details" is clicked. Without
//      them, a connected adapter is marked wifiDetailsWithheld. WlanOpenHandle, WlanEnumInterfaces and
//      the RSSI and channel-number queries are not on that list and are always made. WlanScan and
//      WlanGetAvailableNetworkList, also on it, are never called.
//
// The calls go through a table of function pointers, the system's by default (systemFunctions()), so
// tests feed fake adapter lists and WLAN answers (test_WindowsNetworkAdapters.cpp).

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; their symbols come from sub-headers
// that cannot be included on their own without breaking the required include order.
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include "Platform/ISystemInfoProbe.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Platform::WindowsNetworkAdapters
{

/// The calls readNetworkAdapters() makes. getAdaptersAddresses must be set; the WLAN calls are null when
/// wlanapi.dll (or one of its exports) is missing, and the Wi-Fi connection is then not read. Without the
/// route-table calls, the default gateway is chosen by the interface metric alone.
struct Functions
{
    // NOLINTBEGIN(misc-include-cleaner) - the WLAN and route types come from the umbrella headers' sub-headers
    ULONG(WINAPI* getAdaptersAddresses)(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES addresses, PULONG size) = nullptr;
    /// GetIpForwardTable2 and FreeMibTable, which frees its table.
    DWORD(WINAPI* getIpForwardTable2)(ADDRESS_FAMILY family, PMIB_IPFORWARD_TABLE2* table) = nullptr;
    VOID(WINAPI* freeMibTable)(PVOID memory) = nullptr;
    DWORD(WINAPI* wlanOpenHandle)(DWORD clientVersion, PVOID reserved, PDWORD negotiatedVersion, PHANDLE client) = nullptr;
    DWORD(WINAPI* wlanCloseHandle)(HANDLE client, PVOID reserved) = nullptr;
    DWORD(WINAPI* wlanEnumInterfaces)(HANDLE client, PVOID reserved, PWLAN_INTERFACE_INFO_LIST* list) = nullptr;
    DWORD(WINAPI* wlanQueryInterface)
    (HANDLE client,
     const GUID* interfaceGuid,
     WLAN_INTF_OPCODE opcode,
     PVOID reserved,
     PDWORD size,
     PVOID* data,
     PWLAN_OPCODE_VALUE_TYPE type) = nullptr;
    DWORD(WINAPI* wlanGetNetworkBssList)
    (HANDLE client,
     const GUID* interfaceGuid,
     PDOT11_SSID ssid,
     DOT11_BSS_TYPE bssType,
     BOOL securityEnabled,
     PVOID reserved,
     PWLAN_BSS_LIST* list) = nullptr;
    VOID(WINAPI* wlanFreeMemory)(PVOID memory) = nullptr;
    /// The time now, in Unix seconds, for a DHCP lease's expiry; null or 0 leaves the expiry unknown.
    std::uint64_t (*nowUnix)() = nullptr;
    // NOLINTEND(misc-include-cleaner)
};

/// The real table: iphlpapi's GetAdaptersAddresses, GetIpForwardTable2 and FreeMibTable, and wlanapi.dll's
/// exports when it loads (once per process, from System32).
[[nodiscard]] Functions systemFunctions();

/// Every adapter through @p fns into @p info (available, listed, adapters sorted by name, the default
/// gateways and DNS servers). Never throws for a failed call: what couldn't be read stays empty. The
/// location-gated Wi-Fi calls (the current connection and the BSS list) are made only with
/// @p includeWifiDetails (#1782).
void readNetworkAdapters(NetworkAdaptersInfo& info, const Functions& fns, bool includeWifiDetails);

// ---- Pure helpers, exposed for tests ----

/// A MAC address in Windows' own form ("A4-5E-60-12-34-56"); empty for no address or all zeros.
[[nodiscard]] std::string formatMac(std::span<const std::uint8_t> bytes);

/// An IPv4 or IPv6 socket address's address text ("192.168.1.20", "fe80::1c2a:3bff:fe4d:5e6f"), without
/// a port or scope; empty for a null or other-family address.
[[nodiscard]] std::string formatSocketAddress(const SOCKET_ADDRESS& address);

/// The placeholder DNS servers fec0:0:0:ffff::1, ::2 and ::3 that Windows lists on an adapter with no
/// IPv6 DNS server configured; never a real resolver.
[[nodiscard]] bool isPlaceholderDnsServer(std::string_view address);

/// The PHY type's standard and generation ("802.11ax (Wi-Fi 6)"; "Wi-Fi 6E" at a 6 GHz @p frequencyMhz);
/// empty for one not known.
[[nodiscard]] std::string_view phyName(std::uint32_t dot11PhyType, std::uint32_t frequencyMhz);

} // namespace Platform::WindowsNetworkAdapters
