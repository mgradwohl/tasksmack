#pragma once

// The Linux Network adapters section's facts (#1518), read under an injected root ("/" in the app, a
// fixture tree in tests): each adapter's MAC, MTU, link state and driver from /sys/class/net, the Wi-Fi
// signal level from /proc/net/wireless, the default gateways from /proc/net/route and
// /proc/net/ipv6_route, and the DNS servers from systemd-resolved's upstream resolv.conf or
// /etc/resolv.conf. The addresses come from getifaddrs(), which the probe passes in, so this header
// stays standard-library only and its parsing and fixture tests build and run everywhere. Per-link DNS,
// DHCP leases and the Wi-Fi network name come over D-Bus, through the readers below (#1518).

#include "LinuxOsInfo.h"
#include "LinuxStorage.h"
#include "Nl80211.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace Platform::LinuxNetworkAdapters
{

/// One address getifaddrs() reported, with the adapter it is on.
struct ListedAddress
{
    std::string adapter;
    AdapterAddress address;
};

/// The addresses of every adapter (getifaddrs() in the app; a fixed list in tests).
using AddressLister = std::vector<ListedAddress> (*)();

/// @p text's lines, without their trailing '\r'.
[[nodiscard]] inline std::vector<std::string_view> lines(std::string_view text)
{
    std::vector<std::string_view> out;
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = (end == std::string_view::npos) ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        out.push_back(line);
    }
    return out;
}

/// @p line's whitespace-separated fields.
[[nodiscard]] inline std::vector<std::string_view> fields(std::string_view line)
{
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while ((pos = line.find_first_not_of(" \t", pos)) != std::string_view::npos)
    {
        const std::size_t end = std::min(line.find_first_of(" \t", pos), line.size());
        out.push_back(line.substr(pos, end - pos));
        pos = end;
    }
    return out;
}

/// A /proc/net/route address: eight hex digits of the address in host (little-endian) byte order,
/// "0101A8C0" -> "192.168.1.1"; empty when it isn't one.
[[nodiscard]] inline std::string formatRouteIpv4(std::string_view hex)
{
    std::uint32_t value = 0;
    const char* const first = hex.data();
    const char* const last = first + hex.size();
    const auto [ptr, ec] = std::from_chars(first, last, value, 16);
    if (hex.size() != 8 || ec != std::errc{} || ptr != last)
    {
        return {};
    }
    return std::format("{}.{}.{}.{}", value & 0xFFU, (value >> 8U) & 0xFFU, (value >> 16U) & 0xFFU, (value >> 24U) & 0xFFU);
}

/// Thirty-two hex digits as an IPv6 address in RFC 5952 form: lowercase, leading zeros dropped, the
/// longest run of two or more zero groups (the first, on a tie) written "::". Empty when it isn't one.
[[nodiscard]] inline std::string formatIpv6Hex(std::string_view hex)
{
    if (hex.size() != 32)
    {
        return {};
    }
    std::array<std::uint32_t, 8> groups{};
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        const char* const first = hex.data() + (g * 4);
        const auto [ptr, ec] = std::from_chars(first, first + 4, groups[g], 16);
        if (ec != std::errc{} || ptr != first + 4)
        {
            return {};
        }
    }
    std::size_t bestStart = groups.size();
    std::size_t bestLength = 1; // a single zero group is not compressed
    for (std::size_t start = 0; start < groups.size();)
    {
        std::size_t end = start;
        while (end < groups.size() && groups[end] == 0)
        {
            ++end;
        }
        if (end - start > bestLength)
        {
            bestStart = start;
            bestLength = end - start;
        }
        start = (end == start) ? start + 1 : end;
    }
    std::string out;
    std::size_t g = 0;
    while (g < groups.size())
    {
        if (g == bestStart)
        {
            out += "::";
            g += bestLength; // past the compressed run
            continue;
        }
        if (!out.empty() && !out.ends_with(':'))
        {
            out += ':';
        }
        out += std::format("{:x}", groups[g]);
        ++g;
    }
    return out;
}

/// One default route: its gateway (next hop), the adapter it leaves by, and its metric.
struct DefaultRoute
{
    std::string gateway;
    std::string adapter;
    std::uint64_t metric = 0;
};

/// The IPv4 default routes in /proc/net/route ("Iface Destination Gateway Flags RefCnt Use Metric Mask
/// ..."): each route to 0.0.0.0/0 with a gateway, in file order.
[[nodiscard]] inline std::vector<DefaultRoute> defaultRoutesV4(std::string_view text)
{
    std::vector<DefaultRoute> routes;
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() < 8 || f[1] != "00000000" || f[7] != "00000000" || f[2] == "00000000")
        {
            continue; // the header, a non-default route, or one with no gateway
        }
        std::uint64_t metric = 0;
        std::from_chars(f[6].data(), f[6].data() + f[6].size(), metric, 10);
        if (std::string gateway = formatRouteIpv4(f[2]); !gateway.empty())
        {
            routes.push_back({.gateway = std::move(gateway), .adapter = std::string(f[0]), .metric = metric});
        }
    }
    return routes;
}

/// The IPv6 default routes in /proc/net/ipv6_route ("dest destLen src srcLen nextHop metric refCnt use
/// flags iface", hex): each route to ::/0 with a next hop, in file order.
[[nodiscard]] inline std::vector<DefaultRoute> defaultRoutesV6(std::string_view text)
{
    constexpr std::string_view ZERO = "00000000000000000000000000000000";
    std::vector<DefaultRoute> routes;
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() < 10 || f[0] != ZERO || f[1] != "00" || f[4] == ZERO)
        {
            continue;
        }
        std::uint64_t metric = 0;
        std::from_chars(f[5].data(), f[5].data() + f[5].size(), metric, 16);
        if (std::string gateway = formatIpv6Hex(f[4]); !gateway.empty())
        {
            routes.push_back({.gateway = std::move(gateway), .adapter = std::string(f[9]), .metric = metric});
        }
    }
    return routes;
}

/// The lowest-metric route of @p routes (the first of equals) as its gateway and adapter; empty when none.
[[nodiscard]] inline std::pair<std::string, std::string> bestRoute(const std::vector<DefaultRoute>& routes)
{
    const auto best = std::ranges::min_element(routes, {}, &DefaultRoute::metric);
    return best == routes.end() ? std::pair<std::string, std::string>{} : std::pair{best->gateway, best->adapter};
}

/// The IPv4 default route: the gateway and its adapter, from the lowest-metric route to 0.0.0.0/0.
[[nodiscard]] inline std::pair<std::string, std::string> parseDefaultRouteV4(std::string_view text)
{
    return bestRoute(defaultRoutesV4(text));
}

/// The IPv6 default route: the next hop and its adapter, from the lowest-metric route to ::/0.
[[nodiscard]] inline std::pair<std::string, std::string> parseDefaultRouteV6(std::string_view text)
{
    return bestRoute(defaultRoutesV6(text));
}

/// The gateways of @p adapter's own default routes (#2591): IPv4 then IPv6, each by metric, no repeats.
[[nodiscard]] inline std::vector<std::string>
adapterGateways(std::string_view adapter, std::vector<DefaultRoute> v4, std::vector<DefaultRoute> v6)
{
    std::vector<std::string> gateways;
    for (std::vector<DefaultRoute>* routes : {&v4, &v6})
    {
        std::ranges::stable_sort(*routes, {}, &DefaultRoute::metric);
        for (DefaultRoute& route : *routes)
        {
            if (route.adapter == adapter && std::ranges::find(gateways, route.gateway) == gateways.end())
            {
                gateways.push_back(std::move(route.gateway));
            }
        }
    }
    return gateways;
}

/// An adapter's hardware, from its udev database file (/run/udev/data/n<ifindex>, #2591): the hwdata
/// vendor and model ("Intel Corporation Wi-Fi 6E(802.11ax) AX210/AX1675* 2x2 [Typhoon Peak]"); the model
/// alone when it already starts with the vendor or the vendor is unknown. Empty when udev named neither.
[[nodiscard]] inline std::string adapterDescription(std::string_view udevText)
{
    std::string vendor = LinuxStorage::udevProperty(udevText, "ID_VENDOR_FROM_DATABASE");
    std::string model = LinuxStorage::udevProperty(udevText, "ID_MODEL_FROM_DATABASE");
    if (model.empty())
    {
        return vendor;
    }
    if (vendor.empty() || model.starts_with(vendor))
    {
        return model;
    }
    return vendor + " " + model;
}

/// The "nameserver" and "search" lines of a resolv.conf, in file order.
struct ResolverConfig
{
    std::vector<std::string> servers;
    std::vector<std::string> searchDomains;
};

[[nodiscard]] inline ResolverConfig parseResolvConf(std::string_view text)
{
    ResolverConfig config;
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() >= 2 && f[0] == "nameserver")
        {
            config.servers.emplace_back(f[1]);
        }
        else if (!f.empty() && (f[0] == "search" || f[0] == "domain"))
        {
            config.searchDomains.assign(f.begin() + 1, f.end()); // the last one wins (resolv.conf(5))
        }
    }
    return config;
}

/// The signal level of @p adapter in /proc/net/wireless ("Inter-| sta-| Quality | ... wlan0: 0000  54.
/// -56.  -256 ..."): the dBm level, written with a trailing '.' by most drivers. nullopt when absent.
[[nodiscard]] inline std::optional<int> parseWirelessLevel(std::string_view text, std::string_view adapter)
{
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() < 4 || f[0].size() != adapter.size() + 1 || !f[0].starts_with(adapter) || !f[0].ends_with(':'))
        {
            continue;
        }
        std::string_view level = f[3];
        if (level.ends_with('.'))
        {
            level.remove_suffix(1);
        }
        int dbm = 0;
        const char* const first = level.data();
        const char* const last = first + level.size();
        const auto [ptr, ec] = std::from_chars(first, last, dbm, 10);
        if (ec != std::errc{} || ptr != last || dbm >= 0)
        {
            return std::nullopt; // some drivers report quality only, with level 0
        }
        return dbm;
    }
    return std::nullopt;
}

/// Reads a Wi-Fi adapter's station (its access point's rates) by interface index: Nl80211::readStation(), or
/// a fake in tests. Nullopt when there is none to read.
using WifiStationReader = std::function<std::optional<Nl80211::Station>(int ifindex)>;

/// A Wi-Fi adapter's link as a WifiLinkReader read it: the link, or why there is none.
struct WifiLinkRead
{
    std::optional<WifiLink> link;
    std::string error; ///< Why not, for the UI, when !link: not connected, no NetworkManager, no D-Bus in this build.
};

/// Reads a Wi-Fi adapter's link by its name ("wlp3s0"). SystemdBus::makeWifiLinkReader() asks
/// NetworkManager; tests hand in their own.
using WifiLinkReader = std::function<WifiLinkRead(const std::string& adapter)>;

/// An IP address from its family (AF_INET is 2, AF_INET6 is 10, as Linux numbers them) and its bytes
/// in network order, as systemd-resolved hands them over. Empty when the two don't make an address.
[[nodiscard]] inline std::string formatAddressBytes(int family, std::span<const std::uint8_t> bytes)
{
    constexpr int LINUX_AF_INET = 2;
    constexpr int LINUX_AF_INET6 = 10;
    if (family == LINUX_AF_INET && bytes.size() == 4)
    {
        return std::format("{}.{}.{}.{}", bytes[0], bytes[1], bytes[2], bytes[3]);
    }
    if (family == LINUX_AF_INET6 && bytes.size() == 16)
    {
        std::string hex;
        hex.reserve(32);
        for (const std::uint8_t byte : bytes)
        {
            hex += std::format("{:02x}", byte);
        }
        return formatIpv6Hex(hex);
    }
    return {};
}

/// An adapter's own DNS settings as a LinkDnsReader read them.
struct LinkDnsRead
{
    bool read = false; ///< The settings were read: empty lists then mean the adapter has none of its own.
    std::vector<std::string> servers;
    std::vector<std::string> domains;
    std::string error; ///< Why not, when !read.
};

/// Reads an adapter's own DNS settings by its interface index (/sys/class/net/<name>/ifindex).
/// SystemdBus::makeLinkDnsReader() asks systemd-resolved; tests hand in their own.
using LinkDnsReader = std::function<LinkDnsRead(int ifindex)>;

/// An adapter's IPv4 DHCP lease as a DhcpLeaseReader read it: the lease, or why there is none.
struct DhcpLeaseRead
{
    std::optional<DhcpLease> lease;
    std::string error; ///< Why not, when !lease: no DHCP on it, not managed, no NetworkManager.
    /// Whether the manager configures the adapter's IPv4 by DHCP (#2591): NetworkManager knows either way
    /// for a device it manages; a systemd-networkd lease says yes. Unset when the reader can't tell.
    std::optional<bool> dhcp;
};

/// Reads an adapter's IPv4 DHCP lease by its name. SystemdBus::makeDhcpLeaseReader() asks
/// NetworkManager; tests hand in their own.
using DhcpLeaseReader = std::function<DhcpLeaseRead(const std::string& adapter)>;

/// NetworkManager's DHCP4Config Options, as their string values: the lease from
/// "dhcp_server_identifier", "dhcp_lease_time" (seconds) and "expiry" (Unix seconds). None when no
/// option gives anything.
[[nodiscard]] inline std::optional<DhcpLease>
leaseFromDhcpOptions(std::string_view server, std::string_view leaseTime, std::string_view expiry)
{
    DhcpLease lease;
    lease.server = server;
    std::from_chars(leaseTime.data(), leaseTime.data() + leaseTime.size(), lease.leaseSeconds, 10);
    std::from_chars(expiry.data(), expiry.data() + expiry.size(), lease.expiresUnix, 10);
    if (lease.server.empty() && lease.leaseSeconds == 0 && lease.expiresUnix == 0)
    {
        return std::nullopt;
    }
    return lease;
}

/// Reads an adapter's IPv4 DHCP lease by its interface index, from a source that knows adapters by
/// index: SystemdBus::makeNetworkdLeaseReader() asks systemd-networkd.
using IndexedDhcpLeaseReader = std::function<DhcpLeaseRead(int ifindex)>;

/// The system-wide sources readNetworkAdapterFacts() asks for each adapter; an empty one isn't asked.
struct AdapterReaders
{
    WifiLinkReader wifiLink;              ///< Each Wi-Fi adapter's link.
    LinkDnsReader linkDns;                ///< Each adapter's own DNS servers and search domains.
    DhcpLeaseReader dhcpLease;            ///< Each adapter's IPv4 DHCP lease (NetworkManager).
    IndexedDhcpLeaseReader networkdLease; ///< The same from systemd-networkd, asked when the first has none.
    WifiStationReader wifiStation;        ///< Each connected Wi-Fi adapter's rates (nl80211, #2591).
};

/// The facts under @p root into @p info, with the addresses from @p listAddresses and what @p readers
/// can add: each Wi-Fi adapter's link, each adapter's own DNS settings.
inline void readNetworkAdapterFacts(const std::filesystem::path& root,
                                    NetworkAdaptersInfo& info,
                                    AddressLister listAddresses,
                                    const AdapterReaders& readers = {})
{
    info.available = true;
    std::error_code ec;
    std::filesystem::directory_iterator entries(root / "sys/class/net", ec);
    info.listed = !ec;
    const std::string wireless = LinuxOsInfo::readFile(root / "proc/net/wireless");
    for (; !ec && entries != std::filesystem::directory_iterator{}; entries.increment(ec))
    {
        const std::filesystem::path& dir = entries->path();
        NetworkAdapter adapter;
        adapter.name = dir.filename().string();
        if (adapter.name == "lo")
        {
            continue;
        }
        adapter.mac = LinuxOsInfo::readLine(dir / "address");
        if (adapter.mac == "00:00:00:00:00:00")
        {
            adapter.mac.clear(); // a tunnel or other device with no hardware address
        }
        const std::string mtu = LinuxOsInfo::readLine(dir / "mtu");
        std::from_chars(mtu.data(), mtu.data() + mtu.size(), adapter.mtu, 10);
        adapter.up = LinuxOsInfo::readLine(dir / "operstate") == "up";
        std::error_code linkError;
        const std::filesystem::path driver = std::filesystem::read_symlink(dir / "device/driver", linkError);
        if (!linkError)
        {
            adapter.driver = driver.filename().string();
        }
        int ifindex = 0;
        const std::string index = LinuxOsInfo::readLine(dir / "ifindex");
        std::from_chars(index.data(), index.data() + index.size(), ifindex, 10);
        adapter.wireless = std::filesystem::is_directory(dir / "wireless", linkError);
        if (adapter.wireless)
        {
            adapter.wifiSignalDbm = parseWirelessLevel(wireless, adapter.name);
            if (readers.wifiLink)
            {
                WifiLinkRead read = readers.wifiLink(adapter.name);
                adapter.wifiLink = std::move(read.link);
                adapter.wifiLinkUnavailable = std::move(read.error);
            }
            // The standard and the receive rate, which NetworkManager doesn't give (#2591).
            if (adapter.wifiLink && readers.wifiStation && ifindex > 0)
            {
                if (const std::optional<Nl80211::Station> station = readers.wifiStation(ifindex))
                {
                    WifiLink& link = *adapter.wifiLink;
                    link.phy = Nl80211::standardName(*station, link.frequencyMhz);
                    link.rxBitrateKbps = station->rx ? station->rx->kbps : 0;
                    if (link.bitrateKbps == 0 && station->tx)
                    {
                        link.bitrateKbps = station->tx->kbps;
                    }
                }
            }
        }
        if (ifindex > 0)
        {
            adapter.description = adapterDescription(LinuxOsInfo::readFile(root / "run/udev/data" / ("n" + std::to_string(ifindex))));
        }
        // The lease from whichever manages the adapter: NetworkManager, else systemd-networkd. No lease
        // (a static address, an unmanaged adapter) just leaves the row out.
        if (readers.dhcpLease)
        {
            DhcpLeaseRead read = readers.dhcpLease(adapter.name);
            adapter.dhcpLease = std::move(read.lease);
            adapter.dhcpEnabled = read.dhcp;
        }
        if (!adapter.dhcpLease && readers.networkdLease && ifindex > 0)
        {
            if (DhcpLeaseRead read = readers.networkdLease(ifindex); read.lease)
            {
                adapter.dhcpLease = std::move(read.lease);
                adapter.dhcpEnabled = true;
            }
        }
        if (readers.linkDns && ifindex > 0)
        {
            // A failed read leaves the adapter's rows out: the system-wide DNS servers still show.
            if (LinkDnsRead read = readers.linkDns(ifindex); read.read)
            {
                adapter.linkDnsServers = std::move(read.servers);
                adapter.linkSearchDomains = std::move(read.domains);
            }
        }
        info.adapters.push_back(std::move(adapter));
    }
    std::ranges::sort(info.adapters, {}, &NetworkAdapter::name);

    if (listAddresses != nullptr)
    {
        for (ListedAddress& listed : listAddresses())
        {
            const auto it = std::ranges::find(info.adapters, listed.adapter, &NetworkAdapter::name);
            if (it != info.adapters.end())
            {
                it->addresses.push_back(std::move(listed.address));
            }
        }
    }

    const std::vector<DefaultRoute> routesV4 = defaultRoutesV4(LinuxOsInfo::readFile(root / "proc/net/route"));
    const std::vector<DefaultRoute> routesV6 = defaultRoutesV6(LinuxOsInfo::readFile(root / "proc/net/ipv6_route"));
    std::tie(info.gatewayV4, info.gatewayV4Adapter) = bestRoute(routesV4);
    std::tie(info.gatewayV6, info.gatewayV6Adapter) = bestRoute(routesV6);
    for (NetworkAdapter& adapter : info.adapters)
    {
        adapter.gateways = adapterGateways(adapter.name, routesV4, routesV6);
        // "No DHCP" means a static address only when there is one: an adapter NetworkManager hasn't
        // brought up has no DHCP config either, and isn't static.
        if (adapter.dhcpEnabled == false && std::ranges::none_of(adapter.addresses, [](const AdapterAddress& a) { return !a.v6; }))
        {
            adapter.dhcpEnabled.reset();
        }
    }

    // systemd-resolved keeps the real upstream servers in its own file; /etc/resolv.conf then names only
    // its local stub. Either file alone is used as it is.
    std::string resolv = LinuxOsInfo::readFile(root / "run/systemd/resolve/resolv.conf");
    if (resolv.empty())
    {
        resolv = LinuxOsInfo::readFile(root / "etc/resolv.conf");
    }
    if (!resolv.empty())
    {
        info.dnsRead = true;
        ResolverConfig config = parseResolvConf(resolv);
        info.dnsIsLocalStub = config.servers.size() == 1 && config.servers.front() == "127.0.0.53";
        info.dnsServers = std::move(config.servers);
        info.searchDomains = std::move(config.searchDomains);
    }
}

} // namespace Platform::LinuxNetworkAdapters
