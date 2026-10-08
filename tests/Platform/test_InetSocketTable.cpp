/// @file test_InetSocketTable.cpp
/// @brief Platform::InetSockets (#799): the /proc/net/{tcp,tcp6,udp,udp6} parser behind the
/// Connections section's fallback, on fixtures in the kernel's format -- IPv4 and IPv6 endpoints
/// (each 32-bit word printed in host order), hex ports and states, the inode column, the header and
/// malformed lines -- and the mapping of the kernel's state numbers.

#include "Platform/IProcessConnections.h"
#include "Platform/Linux/InetSocketTable.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string_view>
#include <vector>

namespace Platform::InetSockets
{
namespace
{

/// The fixtures are the bytes a little-endian kernel prints (x86-64, arm64): each address word is a
/// host-order %08X of network-order bytes.
constexpr bool LITTLE_ENDIAN_HOST = std::endian::native == std::endian::little;

constexpr std::string_view TCP_TABLE =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
    "   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 23456 1 0000000000000000 100 0 0 10 0\n"
    "   1: 0F02000A:D6F2 22D8B85D:01BB 01 00000000:00000000 02:000A1B2C 00000000  1000        0 34567 2 0000000000000000 20 4 30 10 -1\n"
    "   2: 0F02000A:D6F4 22D8B85D:01BB 06 00000000:00000000 03:00001234 00000000     0        0 0 3 0000000000000000\n";

constexpr std::string_view TCP6_TABLE = "  sl  local_address                         remote_address                        st tx_queue "
                                        "rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                                        "   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A "
                                        "00000000:00000000 00:00000000 00000000     0        0 11111 1 0000000000000000 100 0 0 10 0\n"
                                        "   1: 00000000000000000000000001000000:0277 00000000000000000000000001000000:9C40 01 "
                                        "00000000:00000000 00:00000000 00000000  1000        0 22222 1 0000000000000000 20 4 30 10 -1\n"
                                        "   2: B80D0120000000000000000001000000:01BB 0000000000000000FFFF0000010200C0:C350 08 "
                                        "00000000:00000000 00:00000000 00000000  1000        0 33333 1 0000000000000000 20 4 30 10 -1\n";

constexpr std::string_view UDP_TABLE =
    "   sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
    "  5: 00000000:14E9 00000000:0000 07 00000000:00000000 00:00000000 00000000  1000        0 44444 2 0000000000000000 0\n"
    "  9: 0F02000A:A1B2 08080808:0035 01 00000000:00000000 00:00000000 00000000  1000        0 55555 2 0000000000000000 0\n";

[[nodiscard]] std::array<std::uint8_t, 16> bytes(std::initializer_list<std::uint8_t> values)
{
    std::array<std::uint8_t, 16> out{};
    std::size_t i = 0;
    for (const std::uint8_t value : values)
    {
        out.at(i++) = value;
    }
    return out;
}

TEST(InetSocketTableTest, MapsEveryKernelTcpState)
{
    EXPECT_EQ(connectionStateFromLinux(1), ConnectionState::Established);
    EXPECT_EQ(connectionStateFromLinux(2), ConnectionState::SynSent);
    EXPECT_EQ(connectionStateFromLinux(3), ConnectionState::SynReceived);
    EXPECT_EQ(connectionStateFromLinux(4), ConnectionState::FinWait1);
    EXPECT_EQ(connectionStateFromLinux(5), ConnectionState::FinWait2);
    EXPECT_EQ(connectionStateFromLinux(6), ConnectionState::TimeWait);
    EXPECT_EQ(connectionStateFromLinux(7), ConnectionState::Closed);
    EXPECT_EQ(connectionStateFromLinux(8), ConnectionState::CloseWait);
    EXPECT_EQ(connectionStateFromLinux(9), ConnectionState::LastAck);
    EXPECT_EQ(connectionStateFromLinux(10), ConnectionState::Listen);
    EXPECT_EQ(connectionStateFromLinux(11), ConnectionState::Closing);
    EXPECT_EQ(connectionStateFromLinux(12), ConnectionState::SynReceived); // TCP_NEW_SYN_RECV
    EXPECT_EQ(connectionStateFromLinux(0), ConnectionState::Unknown);
    EXPECT_EQ(connectionStateFromLinux(13), ConnectionState::Unknown);
}

TEST(InetSocketTableTest, ParsesIPv4TcpRowsAndSkipsTheHeader)
{
    if (!LITTLE_ENDIAN_HOST)
    {
        GTEST_SKIP() << "fixtures are in a little-endian kernel's format";
    }
    std::vector<RawInetSocket> sockets;
    parseProcNetTable(TCP_TABLE, ConnectionProtocol::Tcp, ConnectionFamily::IPv4, sockets);
    ASSERT_EQ(sockets.size(), 3U);

    // 127.0.0.1:631 listening
    EXPECT_EQ(sockets[0].inode, 23456U);
    EXPECT_EQ(sockets[0].protocol, ConnectionProtocol::Tcp);
    EXPECT_EQ(sockets[0].family, ConnectionFamily::IPv4);
    EXPECT_EQ(sockets[0].local.address, bytes({127, 0, 0, 1}));
    EXPECT_EQ(sockets[0].local.port, 631);
    EXPECT_EQ(sockets[0].remote.address, bytes({}));
    EXPECT_EQ(sockets[0].remote.port, 0);
    EXPECT_EQ(toConnection(sockets[0]).state, ConnectionState::Listen);

    // 10.0.2.15:55026 -> 93.184.216.34:443 established
    EXPECT_EQ(sockets[1].inode, 34567U);
    EXPECT_EQ(sockets[1].local.address, bytes({10, 0, 2, 15}));
    EXPECT_EQ(sockets[1].local.port, 55026);
    EXPECT_EQ(sockets[1].remote.address, bytes({93, 184, 216, 34}));
    EXPECT_EQ(sockets[1].remote.port, 443);
    EXPECT_EQ(toConnection(sockets[1]).state, ConnectionState::Established);

    // TIME_WAIT: no file holds it, so inode 0
    EXPECT_EQ(sockets[2].inode, 0U);
    EXPECT_EQ(toConnection(sockets[2]).state, ConnectionState::TimeWait);
}

TEST(InetSocketTableTest, ParsesIPv6RowsWordByWord)
{
    if (!LITTLE_ENDIAN_HOST)
    {
        GTEST_SKIP() << "fixtures are in a little-endian kernel's format";
    }
    std::vector<RawInetSocket> sockets;
    parseProcNetTable(TCP6_TABLE, ConnectionProtocol::Tcp, ConnectionFamily::IPv6, sockets);
    ASSERT_EQ(sockets.size(), 3U);

    // [::]:22 listening
    EXPECT_EQ(sockets[0].family, ConnectionFamily::IPv6);
    EXPECT_EQ(sockets[0].local.address, bytes({}));
    EXPECT_EQ(sockets[0].local.port, 22);
    EXPECT_EQ(toConnection(sockets[0]).state, ConnectionState::Listen);

    // [::1]:631 -> [::1]:40000
    EXPECT_EQ(sockets[1].local.address, bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(sockets[1].remote.port, 40000);
    EXPECT_EQ(sockets[1].inode, 22222U);

    // [2001:db8::1]:443 -> [::ffff:192.0.2.1]:50000 CLOSE_WAIT
    EXPECT_EQ(sockets[2].local.address, bytes({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}));
    EXPECT_EQ(sockets[2].remote.address, bytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 0, 2, 1}));
    EXPECT_EQ(sockets[2].remote.port, 50000);
    EXPECT_EQ(toConnection(sockets[2]).state, ConnectionState::CloseWait);
}

TEST(InetSocketTableTest, ParsesUdpRowsUnconnectedAndConnected)
{
    if (!LITTLE_ENDIAN_HOST)
    {
        GTEST_SKIP() << "fixtures are in a little-endian kernel's format";
    }
    std::vector<RawInetSocket> sockets;
    parseProcNetTable(UDP_TABLE, ConnectionProtocol::Udp, ConnectionFamily::IPv4, sockets);
    ASSERT_EQ(sockets.size(), 2U);
    EXPECT_EQ(sockets[0].protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(sockets[0].local.port, 5353);
    EXPECT_EQ(toConnection(sockets[0]).state, ConnectionState::Closed); // bound, not connected: UNCONN
    EXPECT_EQ(sockets[0].inode, 44444U);
    EXPECT_EQ(sockets[1].remote.address, bytes({8, 8, 8, 8}));
    EXPECT_EQ(sockets[1].remote.port, 53);
    EXPECT_EQ(toConnection(sockets[1]).state, ConnectionState::Established);
}

TEST(InetSocketTableTest, RejectsMalformedLines)
{
    const auto parse = [](std::string_view line, ConnectionFamily family = ConnectionFamily::IPv4)
    {
        return parseProcNetLine(line, ConnectionProtocol::Tcp, family);
    };
    EXPECT_FALSE(parse("").has_value());
    EXPECT_FALSE(parse("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode").has_value());
    // An IPv6-length address in the IPv4 table, and the reverse
    EXPECT_FALSE(parse("   0: 00000000000000000000000000000000:0016 00000000000000000000000000000000:0000 0A 0:0 0:0 0 0 0 1").has_value());
    EXPECT_FALSE(parse("   0: 0100007F:0277 00000000:0000 0A 0:0 0:0 0 0 0 1", ConnectionFamily::IPv6).has_value());
    // Not hex; a port without four digits; no state
    EXPECT_FALSE(parse("   0: 0100007G:0277 00000000:0000 0A 0:0 0:0 0 0 0 1").has_value());
    EXPECT_FALSE(parse("   0: 0100007F:277 00000000:0000 0A 0:0 0:0 0 0 0 1").has_value());
    EXPECT_FALSE(parse("   0: 0100007F:0277 00000000:0000").has_value());
    // Cut off before the inode, or a non-numeric one
    EXPECT_FALSE(parse("   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0").has_value());
    EXPECT_FALSE(parse("   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 12x4").has_value());
    // A well-formed line parses
    const std::optional<RawInetSocket> ok =
        parse("   0: 0100007F:0277 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 77");
    ASSERT_TRUE(ok.has_value());
    EXPECT_EQ(ok.value_or(RawInetSocket{}).inode, 77U);
}

TEST(InetSocketTableTest, ATruncatedLastLineIsSkipped)
{
    if (!LITTLE_ENDIAN_HOST)
    {
        GTEST_SKIP() << "fixtures are in a little-endian kernel's format";
    }
    std::vector<RawInetSocket> sockets;
    parseProcNetTable(TCP_TABLE.substr(0, TCP_TABLE.size() - 60), ConnectionProtocol::Tcp, ConnectionFamily::IPv4, sockets);
    EXPECT_EQ(sockets.size(), 2U);
}

} // namespace
} // namespace Platform::InetSockets
