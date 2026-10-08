/// @file test_ProcessConnections.cpp
/// @brief Domain::Connections (#799): how the Connections section shows a raw socket -- protocol
/// labels, state names (UDP's UNCONN), IPv4 and RFC 5952 IPv6 addresses, wildcard ports -- and the
/// order endpoints and states sort in.

#include "Domain/ProcessConnections.h"
#include "Platform/IProcessConnections.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace Domain::Connections
{
namespace
{

using Platform::ConnectionEndpoint;
using Platform::ConnectionFamily;
using Platform::ConnectionProtocol;
using Platform::ConnectionState;

[[nodiscard]] std::array<std::uint8_t, 16> v4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d)
{
    return {a, b, c, d};
}

/// An IPv6 address from its eight 16-bit groups.
[[nodiscard]] std::array<std::uint8_t, 16> v6(std::initializer_list<std::uint16_t> groups)
{
    std::array<std::uint8_t, 16> bytes{};
    std::size_t i = 0;
    for (const std::uint16_t group : groups)
    {
        bytes.at(i++) = static_cast<std::uint8_t>(group >> 8U);
        bytes.at(i++) = static_cast<std::uint8_t>(group & 0xFFU);
    }
    return bytes;
}

TEST(ProcessConnectionsFormatTest, ProtocolLabelsNameTheTable)
{
    EXPECT_EQ(protocolLabel(ConnectionProtocol::Tcp, ConnectionFamily::IPv4), "TCP");
    EXPECT_EQ(protocolLabel(ConnectionProtocol::Tcp, ConnectionFamily::IPv6), "TCP6");
    EXPECT_EQ(protocolLabel(ConnectionProtocol::Udp, ConnectionFamily::IPv4), "UDP");
    EXPECT_EQ(protocolLabel(ConnectionProtocol::Udp, ConnectionFamily::IPv6), "UDP6");
}

TEST(ProcessConnectionsFormatTest, StateLabelsAreNetstatsAndAnUnconnectedUdpSocketIsUnconn)
{
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::Established), "ESTABLISHED");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::Listen), "LISTEN");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::TimeWait), "TIME_WAIT");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::CloseWait), "CLOSE_WAIT");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::SynSent), "SYN_SENT");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::SynReceived), "SYN_RECV");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::FinWait1), "FIN_WAIT1");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::FinWait2), "FIN_WAIT2");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::LastAck), "LAST_ACK");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::Closing), "CLOSING");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::Closed), "CLOSE");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Tcp, ConnectionState::Unknown), "UNKNOWN");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Udp, ConnectionState::Closed), "UNCONN");
    EXPECT_EQ(stateLabel(ConnectionProtocol::Udp, ConnectionState::Established), "ESTABLISHED");
}

TEST(ProcessConnectionsFormatTest, FormatsIPv4Dotted)
{
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv4, v4(127, 0, 0, 1)), "127.0.0.1");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv4, v4(0, 0, 0, 0)), "0.0.0.0");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv4, v4(255, 255, 255, 255)), "255.255.255.255");
}

TEST(ProcessConnectionsFormatTest, FormatsIPv6InRfc5952Form)
{
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0, 0, 0, 0, 0, 0, 0, 0})), "::");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0, 0, 0, 0, 0, 0, 0, 1})), "::1");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1})), "2001:db8::1");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0xfe80, 0, 0, 0, 0x1234, 0xABCD, 0, 0})), "fe80::1234:abcd:0:0");
    // A single zero group is not compressed; of two equal runs, the first is.
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0x2001, 0xdb8, 0, 1, 1, 1, 1, 1})), "2001:db8:0:1:1:1:1:1");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0x2001, 0, 0, 1, 0, 0, 1, 1})), "2001::1:0:0:1:1");
    // The longer run wins even when it comes second.
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({1, 0, 0, 1, 0, 0, 0, 1})), "1:0:0:1::1");
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({1, 0, 0, 0, 0, 0, 0, 0})), "1::");
}

TEST(ProcessConnectionsFormatTest, FormatsAnIPv4MappedAddressWithItsDottedTail)
{
    EXPECT_EQ(formatAddress(ConnectionFamily::IPv6, v6({0, 0, 0, 0, 0, 0xffff, 0xc000, 0x0201})), "::ffff:192.0.2.1");
}

TEST(ProcessConnectionsFormatTest, EndpointsBracketIPv6AndShowPortZeroAsAStar)
{
    EXPECT_EQ(formatEndpoint(ConnectionFamily::IPv4, ConnectionEndpoint{.address = v4(127, 0, 0, 1), .port = 631}), "127.0.0.1:631");
    EXPECT_EQ(formatEndpoint(ConnectionFamily::IPv4, ConnectionEndpoint{.address = v4(0, 0, 0, 0), .port = 0}), "0.0.0.0:*");
    EXPECT_EQ(formatEndpoint(ConnectionFamily::IPv6, ConnectionEndpoint{.address = v6({0, 0, 0, 0, 0, 0, 0, 0}), .port = 22}), "[::]:22");
    EXPECT_EQ(formatEndpoint(ConnectionFamily::IPv6, ConnectionEndpoint{.address = v6({0x2001, 0xdb8, 0, 0, 0, 0, 0, 1}), .port = 443}),
              "[2001:db8::1]:443");
}

TEST(ProcessConnectionsFormatTest, EndpointsCompareNumericallyNotAsText)
{
    const ConnectionEndpoint nine{.address = v4(10, 0, 0, 9), .port = 80};
    const ConnectionEndpoint ten{.address = v4(10, 0, 0, 10), .port = 80};
    EXPECT_TRUE(compareEndpoints(ConnectionFamily::IPv4, nine, ConnectionFamily::IPv4, ten) < 0);
    const ConnectionEndpoint port80{.address = v4(10, 0, 0, 9), .port = 80};
    const ConnectionEndpoint port443{.address = v4(10, 0, 0, 9), .port = 443};
    EXPECT_TRUE(compareEndpoints(ConnectionFamily::IPv4, port80, ConnectionFamily::IPv4, port443) < 0);
    EXPECT_TRUE(compareEndpoints(ConnectionFamily::IPv4, port80, ConnectionFamily::IPv4, port80) == 0);
    // IPv4 before IPv6, whatever the bytes.
    const ConnectionEndpoint loopback6{.address = v6({0, 0, 0, 0, 0, 0, 0, 1}), .port = 1};
    EXPECT_TRUE(compareEndpoints(ConnectionFamily::IPv4, ten, ConnectionFamily::IPv6, loopback6) < 0);
}

TEST(ProcessConnectionsFormatTest, LiveConnectionsSortFirstThenListenersAndUnknownLast)
{
    EXPECT_LT(stateSortRank(ConnectionState::Established), stateSortRank(ConnectionState::Listen));
    EXPECT_LT(stateSortRank(ConnectionState::Listen), stateSortRank(ConnectionState::Closed));
    EXPECT_LT(stateSortRank(ConnectionState::Closed), stateSortRank(ConnectionState::TimeWait));
    for (const ConnectionState state : {ConnectionState::Established,
                                        ConnectionState::SynSent,
                                        ConnectionState::SynReceived,
                                        ConnectionState::FinWait1,
                                        ConnectionState::FinWait2,
                                        ConnectionState::TimeWait,
                                        ConnectionState::Closed,
                                        ConnectionState::CloseWait,
                                        ConnectionState::LastAck,
                                        ConnectionState::Listen,
                                        ConnectionState::Closing})
    {
        EXPECT_LT(stateSortRank(state), stateSortRank(ConnectionState::Unknown));
    }
}

} // namespace
} // namespace Domain::Connections
