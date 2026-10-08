#pragma once

// Display rules for Process Details' Connections section (#799): Platform::IProcessConnectionsReader
// returns raw sockets (network-order address bytes, port numbers, a platform-neutral state); this turns
// them into what the table shows -- "TCP6", "[2001:db8::1]:443", "ESTABLISHED" -- and the order rows
// sort in. Pure and ImGui-free (tests/Domain/test_ProcessConnections.cpp).

#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

namespace Domain::Connections
{

/// "TCP", "TCP6", "UDP" or "UDP6", as netstat names the tables.
[[nodiscard]] constexpr std::string_view protocolLabel(Platform::ConnectionProtocol protocol, Platform::ConnectionFamily family) noexcept
{
    const bool v6 = family == Platform::ConnectionFamily::IPv6;
    if (protocol == Platform::ConnectionProtocol::Udp)
    {
        return v6 ? "UDP6" : "UDP";
    }
    return v6 ? "TCP6" : "TCP";
}

/// The state as netstat spells it ("ESTABLISHED", "TIME_WAIT", "LISTEN", ...). A UDP socket that is
/// only bound (the kernel's CLOSE) is "UNCONN", as ss calls it; one that connect()ed is "ESTABLISHED".
[[nodiscard]] constexpr std::string_view stateLabel(Platform::ConnectionProtocol protocol, Platform::ConnectionState state) noexcept
{
    using Platform::ConnectionState;
    switch (state)
    {
    case ConnectionState::Established:
        return "ESTABLISHED";
    case ConnectionState::SynSent:
        return "SYN_SENT";
    case ConnectionState::SynReceived:
        return "SYN_RECV";
    case ConnectionState::FinWait1:
        return "FIN_WAIT1";
    case ConnectionState::FinWait2:
        return "FIN_WAIT2";
    case ConnectionState::TimeWait:
        return "TIME_WAIT";
    case ConnectionState::Closed:
        return (protocol == Platform::ConnectionProtocol::Udp) ? "UNCONN" : "CLOSE";
    case ConnectionState::CloseWait:
        return "CLOSE_WAIT";
    case ConnectionState::LastAck:
        return "LAST_ACK";
    case ConnectionState::Listen:
        return "LISTEN";
    case ConnectionState::Closing:
        return "CLOSING";
    case ConnectionState::Unknown:
        break;
    }
    return "UNKNOWN";
}

/// Where a state sorts in the default order: live connections first, then listeners and unconnected
/// UDP sockets, then the handshakes and the ways a connection winds down, unknown last.
[[nodiscard]] constexpr int stateSortRank(Platform::ConnectionState state) noexcept
{
    using Platform::ConnectionState;
    switch (state)
    {
    case ConnectionState::Established:
        return 0;
    case ConnectionState::Listen:
        return 1;
    case ConnectionState::Closed:
        return 2;
    case ConnectionState::SynSent:
        return 3;
    case ConnectionState::SynReceived:
        return 4;
    case ConnectionState::CloseWait:
        return 5;
    case ConnectionState::FinWait1:
        return 6;
    case ConnectionState::FinWait2:
        return 7;
    case ConnectionState::Closing:
        return 8;
    case ConnectionState::LastAck:
        return 9;
    case ConnectionState::TimeWait:
        return 10;
    case ConnectionState::Unknown:
        break;
    }
    return 11;
}

/// The address alone: dotted IPv4 ("192.168.1.10"), or IPv6 in RFC 5952 form -- lowercase hex, no
/// leading zeros, the longest run of two or more zero groups (the first, on a tie) as "::" -- with an
/// IPv4-mapped address in its dotted tail ("::ffff:192.0.2.1").
[[nodiscard]] inline std::string formatAddress(Platform::ConnectionFamily family, const std::array<std::uint8_t, 16>& address)
{
    if (family == Platform::ConnectionFamily::IPv4)
    {
        return std::format("{}.{}.{}.{}", address[0], address[1], address[2], address[3]);
    }

    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        groups.at(i) = static_cast<std::uint16_t>((static_cast<unsigned>(address.at(i * 2)) << 8U) | address.at((i * 2) + 1));
    }

    // ::ffff:a.b.c.d -- an IPv4 peer of a dual-stack socket.
    const bool v4Mapped = std::all_of(groups.begin(), groups.begin() + 5, [](std::uint16_t g) { return g == 0; }) && groups[5] == 0xFFFFU;
    if (v4Mapped)
    {
        return std::format("::ffff:{}.{}.{}.{}", address[12], address[13], address[14], address[15]);
    }

    // The longest run of zero groups, if at least two long.
    std::size_t bestStart = groups.size();
    std::size_t bestLength = 0;
    for (std::size_t i = 0; i < groups.size();)
    {
        if (groups.at(i) != 0)
        {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < groups.size() && groups.at(end) == 0)
        {
            ++end;
        }
        if (end - i > bestLength)
        {
            bestStart = i;
            bestLength = end - i;
        }
        i = end;
    }
    if (bestLength < 2)
    {
        bestStart = groups.size();
        bestLength = 0;
    }

    std::string text;
    for (std::size_t i = 0; i < groups.size();)
    {
        if (i == bestStart)
        {
            text += "::";
            i += bestLength;
            continue;
        }
        if (!text.empty() && !text.ends_with(':'))
        {
            text += ':';
        }
        std::format_to(std::back_inserter(text), "{:x}", groups.at(i));
        ++i;
    }
    return text;
}

/// "address:port", with an IPv6 address in brackets ("[::1]:631") and port 0 -- none yet: a listener's
/// or unconnected socket's remote end, or an unbound local one -- as "*" ("0.0.0.0:*").
[[nodiscard]] inline std::string formatEndpoint(Platform::ConnectionFamily family, const Platform::ConnectionEndpoint& endpoint)
{
    const std::string address = formatAddress(family, endpoint.address);
    const std::string port = (endpoint.port == 0) ? std::string("*") : std::to_string(endpoint.port);
    if (family == Platform::ConnectionFamily::IPv6)
    {
        return std::format("[{}]:{}", address, port);
    }
    return std::format("{}:{}", address, port);
}

/// Orders endpoints numerically: IPv4 before IPv6, then by address bytes, then by port -- so
/// 10.0.0.9 sorts before 10.0.0.10, which a text comparison would not.
[[nodiscard]] inline std::strong_ordering compareEndpoints(Platform::ConnectionFamily familyA,
                                                           const Platform::ConnectionEndpoint& a,
                                                           Platform::ConnectionFamily familyB,
                                                           const Platform::ConnectionEndpoint& b) noexcept
{
    if (const auto order = familyA <=> familyB; order != 0)
    {
        return order;
    }
    if (const auto order = a.address <=> b.address; order != 0)
    {
        return order;
    }
    return a.port <=> b.port;
}

} // namespace Domain::Connections
