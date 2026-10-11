#include "WindowsNetworkAdapters.h"

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsProcAddress.h"
#include "WinString.h"

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers, as in the header
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::WindowsNetworkAdapters
{

namespace
{

// The buffer GetAdaptersAddresses' documentation recommends starting with, and how often to retry
// when the adapters change between its size answer and the copy.
constexpr ULONG INITIAL_BUFFER_BYTES = 15 * 1024;
constexpr int MAX_LIST_ATTEMPTS = 4;

constexpr ULONG LIST_FLAGS = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
constexpr ULONG INFINITE_LEASE = 0xFFFFFFFFU;
constexpr DWORD WLAN_CLIENT_VERSION = 2; // Vista and later

/// Why the Wi-Fi connection couldn't be read, for ERROR_ACCESS_DENIED: Windows 11 24H2 and later gate
/// the network name and BSS list behind location access.
constexpr std::string_view LOCATION_DENIED =
    "Windows refused it: turn on location access for desktop apps (Settings > Privacy & security > Location)";

/// One listed adapter with what readNetworkAdapters() needs beyond the NetworkAdapter itself.
struct Listed
{
    NetworkAdapter adapter;
    std::string guid; ///< AdapterName, "{...}", to match the WLAN interfaces by
    ULONG metricV4 = 0;
    ULONG metricV6 = 0;
    std::vector<std::string> gatewaysV4;
    std::vector<std::string> gatewaysV6;
};

/// The adapter list into @p buffer, retrying while it grows. The GetAdaptersAddresses status.
[[nodiscard]] ULONG listAdapters(const Functions& fns, std::vector<IP_ADAPTER_ADDRESSES>& buffer)
{
    ULONG bytes = INITIAL_BUFFER_BYTES;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    int attempt = 0;
    while (attempt < MAX_LIST_ATTEMPTS && status == ERROR_BUFFER_OVERFLOW)
    {
        buffer.clear();
        buffer.resize((bytes + sizeof(IP_ADAPTER_ADDRESSES) - 1) / sizeof(IP_ADAPTER_ADDRESSES));
        bytes = static_cast<ULONG>(buffer.size() * sizeof(IP_ADAPTER_ADDRESSES));
        status = fns.getAdaptersAddresses(AF_UNSPEC, LIST_FLAGS, nullptr, buffer.data(), &bytes);
        ++attempt;
    }
    return status;
}

[[nodiscard]] std::string wide(const wchar_t* text)
{
    return text == nullptr ? std::string{} : WinString::wideToUtf8(text);
}

/// The adapter's unicast addresses, and the DHCP lease's time left from its DHCP-assigned IPv4 address.
void readUnicast(const IP_ADAPTER_ADDRESSES& raw, NetworkAdapter& adapter)
{
    const IP_ADAPTER_UNICAST_ADDRESS* unicast = raw.FirstUnicastAddress;
    while (unicast != nullptr)
    {
        // Only the addresses in use, as ipconfig shows them: not a tentative or duplicate one, such as the
        // link-local addresses a disconnected adapter keeps.
        const bool inUse = unicast->DadState == IpDadStatePreferred || unicast->DadState == IpDadStateDeprecated;
        std::string text = inUse ? formatSocketAddress(unicast->Address) : std::string{};
        if (!text.empty())
        {
            const bool v6 = unicast->Address.lpSockaddr->sa_family == AF_INET6;
            if (!v6 && unicast->PrefixOrigin == IpPrefixOriginDhcp && unicast->LeaseLifetime != INFINITE_LEASE &&
                !adapter.dhcpLeaseSecondsLeft.has_value())
            {
                adapter.dhcpLeaseSecondsLeft = unicast->LeaseLifetime;
            }
            adapter.addresses.push_back({.address = std::move(text), .prefix = unicast->OnLinkPrefixLength, .v6 = v6});
        }
        unicast = unicast->Next;
    }
}

/// One GetAdaptersAddresses entry.
[[nodiscard]] Listed readAdapter(const IP_ADAPTER_ADDRESSES& raw)
{
    Listed listed;
    NetworkAdapter& adapter = listed.adapter;
    adapter.name = wide(raw.FriendlyName);
    adapter.description = wide(raw.Description);
    adapter.up = raw.OperStatus == IfOperStatusUp;
    adapter.wireless = raw.IfType == IF_TYPE_IEEE80211;
    adapter.mtu = raw.Mtu == std::numeric_limits<ULONG>::max() ? 0 : raw.Mtu;
    adapter.mac =
        formatMac(std::span(raw.PhysicalAddress).first(std::min<std::size_t>(raw.PhysicalAddressLength, MAX_ADAPTER_ADDRESS_LENGTH)));
    adapter.dnsSuffix = wide(raw.DnsSuffix);
    listed.guid = raw.AdapterName == nullptr ? std::string{} : std::string(raw.AdapterName);
    listed.metricV4 = raw.Ipv4Metric;
    listed.metricV6 = raw.Ipv6Metric;

    readUnicast(raw, adapter);

    const IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = raw.FirstGatewayAddress;
    while (gateway != nullptr)
    {
        std::string text = formatSocketAddress(gateway->Address);
        if (!text.empty())
        {
            (gateway->Address.lpSockaddr->sa_family == AF_INET6 ? listed.gatewaysV6 : listed.gatewaysV4).push_back(text);
            adapter.gateways.push_back(std::move(text));
        }
        gateway = gateway->Next;
    }

    const IP_ADAPTER_DNS_SERVER_ADDRESS* dns = raw.FirstDnsServerAddress;
    while (dns != nullptr)
    {
        std::string text = formatSocketAddress(dns->Address);
        if (!text.empty() && !isPlaceholderDnsServer(text) && !std::ranges::contains(adapter.dnsServers, text))
        {
            adapter.dnsServers.push_back(std::move(text));
        }
        dns = dns->Next;
    }

    // Flags rather than the union's bitfields: IP_ADAPTER_IPV4_ENABLED and IP_ADAPTER_DHCP_ENABLED.
    const ULONG flags = raw.Flags; // NOLINT(cppcoreguidelines-pro-type-union-access) - the API's own layout
    if ((flags & IP_ADAPTER_IPV4_ENABLED) != 0)
    {
        const bool dhcp = (flags & IP_ADAPTER_DHCP_ENABLED) != 0;
        adapter.dhcpEnabled = dhcp;
        if (dhcp)
        {
            adapter.dhcpServer = formatSocketAddress(raw.Dhcpv4Server);
        }
        else
        {
            adapter.dhcpLeaseSecondsLeft.reset();
        }
    }
    return listed;
}

/// The default gateway of the up adapter with the lowest metric that has one, as (address, adapter).
template<typename Gateways, typename Metric>
void pickGateway(const std::vector<Listed>& listed, Gateways gateways, Metric metric, std::string& address, std::string& adapter)
{
    const Listed* best = nullptr;
    for (const Listed& entry : listed)
    {
        if (entry.adapter.up && !(entry.*gateways).empty() && (best == nullptr || entry.*metric < best->*metric))
        {
            best = &entry;
        }
    }
    if (best != nullptr)
    {
        address = (best->*gateways).front();
        adapter = best->adapter.name;
    }
}

/// The section-wide DNS servers and search domains: those of the up adapters, lowest metric first.
void collectDns(const std::vector<Listed>& listed, NetworkAdaptersInfo& info)
{
    std::vector<const Listed*> byMetric;
    for (const Listed& entry : listed)
    {
        if (entry.adapter.up)
        {
            byMetric.push_back(&entry);
        }
    }
    std::ranges::stable_sort(byMetric, {}, [](const Listed* entry) { return entry->metricV4; });
    for (const Listed* entry : byMetric)
    {
        for (const std::string& server : entry->adapter.dnsServers)
        {
            if (!std::ranges::contains(info.dnsServers, server))
            {
                info.dnsServers.push_back(server);
            }
        }
        const std::string& suffix = entry->adapter.dnsSuffix;
        if (!suffix.empty() && !std::ranges::contains(info.searchDomains, suffix))
        {
            info.searchDomains.push_back(suffix);
        }
    }
}

/// A WlanQueryInterface answer, freed with WlanFreeMemory when it goes.
class WlanData
{
  public:
    WlanData(const Functions& fns, HANDLE client, const GUID& guid, WLAN_INTF_OPCODE opcode) : m_Fns(&fns)
    {
        DWORD size = 0;
        PVOID data = nullptr;
        m_Status = fns.wlanQueryInterface(client, &guid, opcode, nullptr, &size, &data, nullptr);
        if (m_Status == ERROR_SUCCESS)
        {
            m_Data = data;
            m_Size = size;
        }
    }
    WlanData(const WlanData&) = delete;
    WlanData& operator=(const WlanData&) = delete;
    WlanData(WlanData&&) = delete;
    WlanData& operator=(WlanData&&) = delete;
    ~WlanData()
    {
        if (m_Data != nullptr)
        {
            m_Fns->wlanFreeMemory(m_Data);
        }
    }

    [[nodiscard]] DWORD status() const noexcept
    {
        return m_Status;
    }

    /// The answer as a T, or null when the call failed or the answer is too small for one.
    template<typename T> [[nodiscard]] const T* as() const noexcept
    {
        return m_Data != nullptr && m_Size >= sizeof(T) ? static_cast<const T*>(m_Data) : nullptr;
    }

  private:
    const Functions* m_Fns;
    DWORD m_Status = ERROR_SUCCESS;
    PVOID m_Data = nullptr;
    DWORD m_Size = 0;
};

/// The connected BSS's centre frequency in MHz from the cached BSS list; 0 when it isn't there.
[[nodiscard]] std::uint32_t
connectedFrequencyMhz(const Functions& fns, HANDLE client, const GUID& guid, const WLAN_CONNECTION_ATTRIBUTES& connection)
{
    DOT11_SSID ssid = connection.wlanAssociationAttributes.dot11Ssid;
    PWLAN_BSS_LIST list = nullptr;
    const DWORD status = fns.wlanGetNetworkBssList(client,
                                                   &guid,
                                                   &ssid,
                                                   connection.wlanAssociationAttributes.dot11BssType,
                                                   connection.wlanSecurityAttributes.bSecurityEnabled,
                                                   nullptr,
                                                   &list);
    if (status != ERROR_SUCCESS || list == nullptr)
    {
        return 0;
    }
    std::uint32_t mhz = 0;
    const std::span<const WLAN_BSS_ENTRY> entries(&list->wlanBssEntries[0], list->dwNumberOfItems);
    for (const WLAN_BSS_ENTRY& entry : entries)
    {
        if (std::ranges::equal(entry.dot11Bssid, connection.wlanAssociationAttributes.dot11Bssid))
        {
            mhz = static_cast<std::uint32_t>(entry.ulChCenterFrequency / 1000); // kHz
            break;
        }
    }
    fns.wlanFreeMemory(list);
    return mhz;
}

/// The Wi-Fi connection of the connected interface @p guid into @p adapter.
void readWifi(const Functions& fns, HANDLE client, const GUID& guid, NetworkAdapter& adapter)
{
    WifiConnection wifi;
    bool any = false;
    {
        const WlanData connection(fns, client, guid, wlan_intf_opcode_current_connection);
        const auto* attributes = connection.as<WLAN_CONNECTION_ATTRIBUTES>();
        if (attributes != nullptr && attributes->isState == wlan_interface_state_connected)
        {
            const WLAN_ASSOCIATION_ATTRIBUTES& association = attributes->wlanAssociationAttributes;
            const std::size_t length = std::min<std::size_t>(association.dot11Ssid.uSSIDLength, DOT11_SSID_MAX_LENGTH);
            const std::span<const UCHAR> ssid = std::span(association.dot11Ssid.ucSSID).first(length);
            wifi.ssid.assign(ssid.begin(), ssid.end());
            wifi.signalQuality = static_cast<std::uint32_t>(association.wlanSignalQuality);
            wifi.rxKbps = static_cast<std::uint32_t>(association.ulRxRate);
            wifi.txKbps = static_cast<std::uint32_t>(association.ulTxRate);
            const std::uint32_t mhz = connectedFrequencyMhz(fns, client, guid, *attributes);
            wifi.band = std::string(bandName(mhz));
            wifi.channel = channelOf(mhz);
            wifi.phy = std::string(phyName(static_cast<std::uint32_t>(association.dot11PhyType), mhz));
            any = true;
        }
        else if (connection.status() == ERROR_ACCESS_DENIED)
        {
            adapter.wifiUnavailableReason = LOCATION_DENIED;
        }
    }
    {
        const WlanData rssi(fns, client, guid, wlan_intf_opcode_rssi);
        if (const auto* dbm = rssi.as<LONG>(); dbm != nullptr && *dbm < 0)
        {
            adapter.wifiSignalDbm = static_cast<int>(*dbm);
        }
    }
    if (wifi.channel == 0)
    {
        const WlanData channel(fns, client, guid, wlan_intf_opcode_channel_number);
        if (const auto* number = channel.as<ULONG>(); number != nullptr && *number != 0)
        {
            wifi.channel = static_cast<std::uint32_t>(*number);
            any = true;
        }
    }
    if (any)
    {
        adapter.wifi = std::move(wifi);
    }
}

/// Every connected Wi-Fi interface's connection, into the adapter with its GUID.
void readWlan(const Functions& fns, std::vector<Listed>& listed)
{
    if (fns.wlanOpenHandle == nullptr || fns.wlanCloseHandle == nullptr || fns.wlanEnumInterfaces == nullptr ||
        fns.wlanQueryInterface == nullptr || fns.wlanGetNetworkBssList == nullptr || fns.wlanFreeMemory == nullptr)
    {
        return; // no wlanapi.dll: the Wi-Fi rows are left out
    }
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    if (fns.wlanOpenHandle(WLAN_CLIENT_VERSION, nullptr, &negotiated, &client) != ERROR_SUCCESS)
    {
        return; // the Wireless LAN Service isn't running
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (fns.wlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces != nullptr)
    {
        const std::span<const WLAN_INTERFACE_INFO> infos(&interfaces->InterfaceInfo[0], interfaces->dwNumberOfItems);
        for (const WLAN_INTERFACE_INFO& info : infos)
        {
            const std::string guid = guidText(info.InterfaceGuid);
            const auto match =
                std::ranges::find_if(listed,
                                     [&guid](const Listed& entry)
                                     { return entry.guid.size() == guid.size() && _stricmp(entry.guid.c_str(), guid.c_str()) == 0; });
            if (match == listed.end())
            {
                continue;
            }
            match->adapter.wireless = true;
            if (info.isState == wlan_interface_state_connected)
            {
                readWifi(fns, client, info.InterfaceGuid, match->adapter);
            }
        }
        fns.wlanFreeMemory(interfaces);
    }
    fns.wlanCloseHandle(client, nullptr);
}

ULONG WINAPI systemGetAdaptersAddresses(ULONG family, ULONG flags, PVOID reserved, PIP_ADAPTER_ADDRESSES addresses, PULONG size)
{
    return GetAdaptersAddresses(family, flags, reserved, addresses, size);
}

} // namespace

Functions systemFunctions()
{
    // Loaded once and kept: the read runs again on every Refresh. Missing on Server without the
    // Wireless LAN Service, which leaves the WLAN calls null.
    static const HMODULE wlan = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    Functions fns{.getAdaptersAddresses = &systemGetAdaptersAddresses};
    if (wlan != nullptr)
    {
        fns.wlanOpenHandle = Windows::getProcAddress<decltype(fns.wlanOpenHandle)>(wlan, "WlanOpenHandle");
        fns.wlanCloseHandle = Windows::getProcAddress<decltype(fns.wlanCloseHandle)>(wlan, "WlanCloseHandle");
        fns.wlanEnumInterfaces = Windows::getProcAddress<decltype(fns.wlanEnumInterfaces)>(wlan, "WlanEnumInterfaces");
        fns.wlanQueryInterface = Windows::getProcAddress<decltype(fns.wlanQueryInterface)>(wlan, "WlanQueryInterface");
        fns.wlanGetNetworkBssList = Windows::getProcAddress<decltype(fns.wlanGetNetworkBssList)>(wlan, "WlanGetNetworkBssList");
        fns.wlanFreeMemory = Windows::getProcAddress<decltype(fns.wlanFreeMemory)>(wlan, "WlanFreeMemory");
    }
    return fns;
}

void readNetworkAdapters(NetworkAdaptersInfo& info, const Functions& fns)
{
    info.available = true;
    info.family = OsFamily::Windows;
    std::vector<IP_ADAPTER_ADDRESSES> buffer;
    const ULONG status = listAdapters(fns, buffer);
    if (status == ERROR_NO_DATA)
    {
        info.listed = true; // no adapters at all
        info.dnsRead = true;
        return;
    }
    if (status != ERROR_SUCCESS)
    {
        return; // listed and dnsRead stay false: the section says the list couldn't be read
    }
    info.listed = true;
    info.dnsRead = true;

    std::vector<Listed> listed;
    const IP_ADAPTER_ADDRESSES* raw = buffer.data();
    while (raw != nullptr)
    {
        if (raw->IfType != IF_TYPE_SOFTWARE_LOOPBACK)
        {
            listed.push_back(readAdapter(*raw));
        }
        raw = raw->Next;
    }

    pickGateway(listed, &Listed::gatewaysV4, &Listed::metricV4, info.gatewayV4, info.gatewayV4Adapter);
    pickGateway(listed, &Listed::gatewaysV6, &Listed::metricV6, info.gatewayV6, info.gatewayV6Adapter);
    collectDns(listed, info);
    readWlan(fns, listed);

    info.adapters.reserve(listed.size());
    for (Listed& entry : listed)
    {
        info.adapters.push_back(std::move(entry.adapter));
    }
    std::ranges::sort(info.adapters, {}, &NetworkAdapter::name);
}

std::string formatMac(std::span<const std::uint8_t> bytes)
{
    if (std::ranges::all_of(bytes, [](std::uint8_t byte) { return byte == 0; }))
    {
        return {}; // also an empty span
    }
    std::string text;
    for (const std::uint8_t byte : bytes)
    {
        text += std::format("{}{:02X}", text.empty() ? "" : "-", byte);
    }
    return text;
}

std::string formatSocketAddress(const SOCKET_ADDRESS& address)
{
    if (address.lpSockaddr == nullptr)
    {
        return {};
    }
    std::array<char, INET6_ADDRSTRLEN> text{};
    const ADDRESS_FAMILY family = address.lpSockaddr->sa_family;
    if (family == AF_INET && std::cmp_greater_equal(address.iSockaddrLength, sizeof(sockaddr_in)))
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sa_family says which sockaddr it is
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address.lpSockaddr);
        return inet_ntop(AF_INET, &v4->sin_addr, text.data(), text.size()) == nullptr ? std::string{} : std::string(text.data());
    }
    if (family == AF_INET6 && std::cmp_greater_equal(address.iSockaddrLength, sizeof(sockaddr_in6)))
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) - sa_family says which sockaddr it is
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address.lpSockaddr);
        return inet_ntop(AF_INET6, &v6->sin6_addr, text.data(), text.size()) == nullptr ? std::string{} : std::string(text.data());
    }
    return {};
}

bool isPlaceholderDnsServer(std::string_view address)
{
    return address == "fec0:0:0:ffff::1" || address == "fec0:0:0:ffff::2" || address == "fec0:0:0:ffff::3";
}

std::string_view bandName(std::uint32_t frequencyMhz)
{
    if (frequencyMhz >= 2400 && frequencyMhz < 2500)
    {
        return "2.4 GHz";
    }
    if (frequencyMhz >= 5150 && frequencyMhz < 5925)
    {
        return "5 GHz";
    }
    if (frequencyMhz >= 5925 && frequencyMhz <= 7125)
    {
        return "6 GHz";
    }
    return {};
}

std::uint32_t channelOf(std::uint32_t frequencyMhz)
{
    if (frequencyMhz == 2484)
    {
        return 14;
    }
    if (frequencyMhz >= 2412 && frequencyMhz < 2484)
    {
        return (frequencyMhz - 2407) / 5;
    }
    if (frequencyMhz >= 5150 && frequencyMhz < 5925)
    {
        return (frequencyMhz - 5000) / 5;
    }
    if (frequencyMhz == 5935)
    {
        return 2; // 6 GHz channel 2 sits below channel 1
    }
    if (frequencyMhz >= 5950 && frequencyMhz <= 7125)
    {
        return (frequencyMhz - 5950) / 5;
    }
    return 0;
}

std::string_view phyName(std::uint32_t dot11PhyType, std::uint32_t frequencyMhz)
{
    switch (dot11PhyType)
    {
    case dot11_phy_type_ofdm:
        return "802.11a";
    case dot11_phy_type_hrdsss:
        return "802.11b";
    case dot11_phy_type_erp:
        return "802.11g";
    case dot11_phy_type_ht:
        return "802.11n (Wi-Fi 4)";
    case dot11_phy_type_vht:
        return "802.11ac (Wi-Fi 5)";
    case dot11_phy_type_dmg:
        return "802.11ad (WiGig)";
    case dot11_phy_type_he:
        return bandName(frequencyMhz) == "6 GHz" ? "802.11ax (Wi-Fi 6E)" : "802.11ax (Wi-Fi 6)";
    case dot11_phy_type_eht:
        return "802.11be (Wi-Fi 7)";
    default:
        return {};
    }
}

std::string guidText(const GUID& guid)
{
    return std::format("{{{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}}}",
                       guid.Data1,
                       guid.Data2,
                       guid.Data3,
                       guid.Data4[0],
                       guid.Data4[1],
                       guid.Data4[2],
                       guid.Data4[3],
                       guid.Data4[4],
                       guid.Data4[5],
                       guid.Data4[6],
                       guid.Data4[7]);
}

} // namespace Platform::WindowsNetworkAdapters
