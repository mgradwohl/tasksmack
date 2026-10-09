#pragma once

// The Linux Network adapters section's facts (#1518), read under an injected root ("/" in the app, a
// fixture tree in tests): each adapter's MAC, MTU, link state and driver from /sys/class/net, the Wi-Fi
// signal level from /proc/net/wireless, the default gateways from /proc/net/route and
// /proc/net/ipv6_route, and the DNS servers from systemd-resolved's upstream resolv.conf or
// /etc/resolv.conf. The addresses come from getifaddrs(), which the probe passes in, so this header
// stays standard-library only and its parsing and fixture tests build and run everywhere. No D-Bus:
// per-link DNS, DHCP leases and the Wi-Fi network name are a follow-up.

#include "LinuxOsInfo.h"
#include "Platform/ISystemInfoProbe.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
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

/// The IPv4 default route in /proc/net/route ("Iface Destination Gateway Flags RefCnt Use Metric Mask
/// ..."): the gateway and its adapter, from the lowest-metric route to 0.0.0.0/0. Empty when there is none.
[[nodiscard]] inline std::pair<std::string, std::string> parseDefaultRouteV4(std::string_view text)
{
    std::pair<std::string, std::string> best;
    std::uint64_t bestMetric = UINT64_MAX;
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() < 8 || f[1] != "00000000" || f[7] != "00000000" || f[2] == "00000000")
        {
            continue; // the header, a non-default route, or one with no gateway
        }
        std::uint64_t metric = 0;
        std::from_chars(f[6].data(), f[6].data() + f[6].size(), metric, 10);
        if (const std::string gateway = formatRouteIpv4(f[2]); !gateway.empty() && metric < bestMetric)
        {
            bestMetric = metric;
            best = {gateway, std::string(f[0])};
        }
    }
    return best;
}

/// The IPv6 default route in /proc/net/ipv6_route ("dest destLen src srcLen nextHop metric refCnt use
/// flags iface", hex): the next hop and its adapter, from the lowest-metric route to ::/0 with a next hop.
[[nodiscard]] inline std::pair<std::string, std::string> parseDefaultRouteV6(std::string_view text)
{
    constexpr std::string_view ZERO = "00000000000000000000000000000000";
    std::pair<std::string, std::string> best;
    std::uint64_t bestMetric = UINT64_MAX;
    for (const std::string_view line : lines(text))
    {
        const std::vector<std::string_view> f = fields(line);
        if (f.size() < 10 || f[0] != ZERO || f[1] != "00" || f[4] == ZERO)
        {
            continue;
        }
        std::uint64_t metric = 0;
        std::from_chars(f[5].data(), f[5].data() + f[5].size(), metric, 16);
        if (const std::string gateway = formatIpv6Hex(f[4]); !gateway.empty() && metric < bestMetric)
        {
            bestMetric = metric;
            best = {gateway, std::string(f[9])};
        }
    }
    return best;
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

/// The facts under @p root into @p info, with the addresses from @p listAddresses.
inline void readNetworkAdapterFacts(const std::filesystem::path& root, NetworkAdaptersInfo& info, AddressLister listAddresses)
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
        adapter.wireless = std::filesystem::is_directory(dir / "wireless", linkError);
        if (adapter.wireless)
        {
            adapter.wifiSignalDbm = parseWirelessLevel(wireless, adapter.name);
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

    std::tie(info.gatewayV4, info.gatewayV4Adapter) = parseDefaultRouteV4(LinuxOsInfo::readFile(root / "proc/net/route"));
    std::tie(info.gatewayV6, info.gatewayV6Adapter) = parseDefaultRouteV6(LinuxOsInfo::readFile(root / "proc/net/ipv6_route"));

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
