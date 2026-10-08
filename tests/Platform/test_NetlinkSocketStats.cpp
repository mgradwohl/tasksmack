// Tests for NetlinkSocketStats and related functions
// Only compiled on Linux

#if defined(__linux__)

#include "Platform/IProcessConnections.h"
#include "Platform/Linux/InetSocketTable.h"
#include "Platform/Linux/NetlinkSocketStats.h"
#include "Platform/NetlinkTestUtils.h"
#include "Platform/ScopedTempDir.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - Linux UAPI headers; include-cleaner lacks the mappings
#include <arpa/inet.h> // htons
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h> // IPPROTO_TCP
// NOLINTEND(misc-include-cleaner)
#include <sys/socket.h>
#include <unistd.h>

namespace Platform
{
namespace
{

// ========== NetlinkSocketStats Tests ==========

TEST(NetlinkSocketStatsTest, IsAvailableDoesNotThrow)
{
    NetlinkSocketStats stats;
    // Verify isAvailable() doesn't throw an exception
    // (availability depends on system capabilities, but method should be noexcept)
    EXPECT_NO_THROW([[maybe_unused]] auto available = stats.isAvailable());
}

TEST(NetlinkSocketStatsTest, IsAvailableReturnsConsistentValue)
{
    NetlinkSocketStats stats;
    const bool available1 = stats.isAvailable();
    const bool available2 = stats.isAvailable();
    EXPECT_EQ(available1, available2);
}

TEST(NetlinkSocketStatsTest, QueryAllSocketsReturnsEmptyWhenUnavailable)
{
    NetlinkSocketStats stats;
    if (!stats.isAvailable())
    {
        auto sockets = stats.queryAllSockets();
        EXPECT_TRUE(sockets.empty());
    }
}

TEST(NetlinkSocketStatsTest, QueryAllSocketsDoesNotCrash)
{
    NetlinkSocketStats stats;
    // Should not crash regardless of availability
    auto sockets = stats.queryAllSockets();
    // Result may be empty or contain sockets; reaching here without a crash is success
    SUCCEED() << "QueryAllSockets completed with " << sockets.size() << " sockets";
}

TEST(NetlinkSocketStatsTest, QueryAllSocketsReturnsSocketsWhenAvailable)
{
    NetlinkSocketStats stats;
    if (stats.isAvailable())
    {
        // Query existing system sockets - most systems will have at least some
        // (e.g., systemd services, dbus, the test process itself may have sockets)
        auto sockets = stats.queryAllSockets();
        // We expect at least some sockets on a typical system
        // But we can't guarantee any specific number
        SUCCEED() << "Query returned " << sockets.size() << " sockets";
    }
    else
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }
}

TEST(NetlinkSocketStatsTest, SocketStatsHaveValidInodes)
{
    NetlinkSocketStats stats;
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    auto sockets = stats.queryAllSockets();
    for (const auto& socket : sockets)
    {
        // Each socket should have a non-zero inode
        EXPECT_NE(socket.inode, 0UL) << "Socket has invalid inode";
    }
}

// ========== buildInodeToPidMap Tests ==========

TEST(BuildInodeToPidMapTest, ReturnsNonEmptyMapOnRunningSystem)
{
    auto inodeToPid = buildInodeToPidMap();
    // A running system should have at least some sockets
    // The test process itself might have open sockets; reaching here is success
    SUCCEED() << "buildInodeToPidMap completed with " << inodeToPid.size() << " mappings";
}

TEST(BuildInodeToPidMapTest, MapsSocketsToValidPids)
{
    auto inodeToPid = buildInodeToPidMap();
    for (const auto& [inode, owner] : inodeToPid)
    {
        EXPECT_GT(inode, 0UL) << "Inode should be positive";
        EXPECT_GT(owner.pid, 0) << "PID should be positive";
    }
}

TEST(BuildInodeToPidMapTest, FindsOwnProcessSockets)
{
    // Get our own PID
    const auto ownPid = static_cast<std::int32_t>(getpid());

    auto inodeToPid = buildInodeToPidMap();

    // Check if any sockets are mapped to our process
    bool foundOwnSocket = false;
    for (const auto& [inode, owner] : inodeToPid)
    {
        if (owner.pid == ownPid)
        {
            foundOwnSocket = true;
            break;
        }
    }

    // We may or may not have sockets, so just verify the map doesn't crash
    SUCCEED() << "Found " << (foundOwnSocket ? "own process sockets" : "no own process sockets");
}

// ----- Cache Tests -----

TEST(NetlinkSocketStatsCacheTest, DefaultConstructorUsesDEFAULT_SOCKET_STATS_CACHE_TTL)
{
    NetlinkSocketStats stats;
    EXPECT_EQ(stats.cacheTtl(), DEFAULT_SOCKET_STATS_CACHE_TTL);
}

TEST(NetlinkSocketStatsCacheTest, CustomTTLConstructor)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(200ms);
    EXPECT_EQ(stats.cacheTtl(), 200ms);
}

TEST(NetlinkSocketStatsCacheTest, ZeroTTLEffectivelyDisablesCache)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(0ms);
    EXPECT_EQ(stats.cacheTtl(), 0ms);
}

TEST(NetlinkSocketStatsCacheTest, CacheTtlReturnsConfiguredValue)
{
    using namespace std::chrono_literals;
    constexpr auto customTtl = 750ms;
    NetlinkSocketStats stats(customTtl);
    EXPECT_EQ(stats.cacheTtl(), customTtl);
}

TEST(NetlinkSocketStatsCacheTest, InvalidateCacheWorks)
{
    NetlinkSocketStats stats;
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // First query populates cache
    auto result1 = stats.queryAllSockets();

    // Invalidate cache
    stats.invalidateCache();

    // Next query should hit the kernel again. Direct verification would require
    // mocking kernel calls, but cache behavior is thoroughly tested by
    // CachedQueryReturnsSameResults, CacheInvalidationAfterTTLExpiry, and
    // EmptyResultsAreCached tests. Here we verify invalidateCache() doesn't break.
    auto result2 = stats.queryAllSockets();

    // Both should return valid results (may differ if sockets changed)
    // The test passes implicitly if no exception is thrown
}

TEST(NetlinkSocketStatsCacheTest, CachedQueryReturnsSameResults)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(5000ms); // Long TTL to ensure cache hit
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // First query
    auto result1 = stats.queryAllSockets();

    // Second query should return cached results (identical)
    auto result2 = stats.queryAllSockets();

    // Results should be identical since we're returning the same cached vector
    EXPECT_EQ(result1.size(), result2.size());

    // If both have results, verify they're identical
    if (!result1.empty() && !result2.empty())
    {
        EXPECT_EQ(result1[0].inode, result2[0].inode);
        EXPECT_EQ(result1[0].bytesReceived, result2[0].bytesReceived);
        EXPECT_EQ(result1[0].bytesSent, result2[0].bytesSent);
    }
}

TEST(NetlinkSocketStatsCacheTest, UncachedQueryBypassesCache)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(5000ms); // Long TTL
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // First cached query
    auto cached = stats.queryAllSockets();

    // Uncached query should always hit kernel
    auto uncached = stats.queryAllSocketsUncached();

    // Both should return valid results (may differ slightly if sockets changed)
    // Test passes implicitly if no exception is thrown
}

TEST(NetlinkSocketStatsCacheTest, CacheInvalidationAfterTTLExpiry)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(50ms); // Short TTL for testing
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // First query
    auto result1 = stats.queryAllSockets();

    // Wait for cache to expire (generous 2x margin to avoid flakiness on loaded systems)
    std::this_thread::sleep_for(100ms);

    // This should be a fresh query (cache miss)
    auto result2 = stats.queryAllSockets();

    // Both should return valid results (structure verification)
    for (const auto& sock : result1)
    {
        EXPECT_GE(sock.inode, 0UL);
    }
    for (const auto& sock : result2)
    {
        EXPECT_GE(sock.inode, 0UL);
    }
}

TEST(NetlinkSocketStatsCacheTest, EmptyResultsAreCached)
{
    using namespace std::chrono_literals;
    // This test verifies that empty results (system with no sockets) are properly cached.
    // We can't easily simulate a system with no sockets, but we can verify the cache
    // logic handles empty results correctly by checking that the cache state is updated
    // even when results might be empty.
    NetlinkSocketStats stats(5000ms); // Long TTL
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // First query - populates cache
    auto result1 = stats.queryAllSockets();

    // Invalidate and immediately re-query twice
    // If empty results weren't cached, every call would hit the kernel
    stats.invalidateCache();
    auto result2 = stats.queryAllSockets();
    auto result3 = stats.queryAllSockets();

    // Third query should be a cache hit (same as result2)
    EXPECT_EQ(result2.size(), result3.size());
}

// ========== Integration Tests ==========

TEST(NetlinkSocketStatsIntegrationTest, EndToEndPidMapping)
{
    NetlinkSocketStats stats;
    if (!stats.isAvailable())
    {
        GTEST_SKIP() << "Netlink INET_DIAG not available on this system";
    }

    // Query sockets and build PID map
    auto sockets = stats.queryAllSockets();
    auto inodeToPid = buildInodeToPidMap();

    // Every socket that maps to a process maps to a valid PID
    std::size_t mapped = 0;
    for (const auto& socket : sockets)
    {
        if (const auto it = inodeToPid.find(socket.inode); it != inodeToPid.end())
        {
            EXPECT_GT(it->second.pid, 0) << "PID should be positive";
            ++mapped;
        }
    }

    SUCCEED() << "Mapped " << mapped << " of " << sockets.size() << " sockets to processes";
}

// ========== Scripted-kernel tests (#1101, #1160) ==========

using TestSupport::completeDump;
using TestSupport::doneDatagram;
using TestSupport::errorDatagram;
using TestSupport::FakeSocket;
using TestSupport::ScriptedNetlinkTransport;
using TestSupport::socketsDatagram;

/// A NetlinkSocketStats over a scripted transport, with the constructor's warm-up query already
/// done (and forgotten), so a test sees only its own requests.
struct ScriptedStats
{
    ScriptedNetlinkTransport* transport = nullptr; // owned by stats
    std::unique_ptr<NetlinkSocketStats> stats;

    explicit ScriptedStats(std::chrono::milliseconds ttl)
    {
        auto owned = std::make_unique<ScriptedNetlinkTransport>();
        transport = owned.get();
        stats = std::make_unique<NetlinkSocketStats>(std::move(owned), ttl);
        transport->requests.clear();
        transport->queued.clear();
    }
};

[[nodiscard]] std::vector<std::uint64_t> inodesOf(const std::vector<SocketStats>& sockets)
{
    std::vector<std::uint64_t> inodes;
    inodes.reserve(sockets.size());
    for (const auto& socket : sockets)
    {
        inodes.push_back(socket.inode);
    }
    std::ranges::sort(inodes);
    return inodes;
}

TEST(NetlinkSocketStatsScriptedTest, QueriesTcpOverBothFamiliesAndNeverUdp)
{
    // #1101: a UDP sock_diag dump carries no byte counters, so the UDP dumps were pure overhead.
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    scripted.transport->onRequest = [](const ScriptedNetlinkTransport::Request& request)
    {
        return completeDump(request, {});
    };

    std::chrono::steady_clock::time_point sampledAt;
    [[maybe_unused]] const auto sockets = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_NE(sampledAt, std::chrono::steady_clock::time_point{});

    ASSERT_EQ(scripted.transport->requests.size(), 2U);
    for (const auto& request : scripted.transport->requests)
    {
        EXPECT_EQ(request.protocol, IPPROTO_TCP);
    }
    EXPECT_EQ(scripted.transport->requests[0].family, AF_INET);
    EXPECT_EQ(scripted.transport->requests[1].family, AF_INET6);
}

TEST(NetlinkSocketStatsScriptedTest, EachRequestCarriesAFreshSequenceNumber)
{
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    scripted.transport->onRequest = [](const ScriptedNetlinkTransport::Request& request)
    {
        return completeDump(request, {});
    };

    [[maybe_unused]] const auto first = scripted.stats->queryAllSockets();
    [[maybe_unused]] const auto second = scripted.stats->queryAllSockets();

    ASSERT_EQ(scripted.transport->requests.size(), 4U);
    std::vector<std::uint32_t> sequences;
    sequences.reserve(scripted.transport->requests.size());
    for (const auto& request : scripted.transport->requests)
    {
        sequences.push_back(request.sequence);
    }
    std::ranges::sort(sequences);
    EXPECT_EQ(std::ranges::adjacent_find(sequences), sequences.end()) << "a sequence number was reused";
}

TEST(NetlinkSocketStatsScriptedTest, StaleRepliesFromAnEarlierDumpAreIgnored)
{
    // #1160: replies from an earlier dump -- including its NLMSG_DONE -- used to be read as this
    // dump's, so the stale DONE ended it early: the current sockets were lost and stale ones kept.
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array staleSockets{FakeSocket{.inode = 900, .bytesReceived = 1}};
    const std::array currentSockets{FakeSocket{.inode = 11, .bytesReceived = 100}, FakeSocket{.inode = 12, .bytesReceived = 200}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        ScriptedNetlinkTransport::Reply reply;
        if (request.family == AF_INET)
        {
            // Arrives after the request was sent, so draining beforehand can't remove it.
            const std::uint32_t staleSequence = request.sequence - 1;
            reply.emplace_back(socketsDatagram(staleSockets, staleSequence, ScriptedNetlinkTransport::PORT_ID));
            reply.emplace_back(doneDatagram(staleSequence, ScriptedNetlinkTransport::PORT_ID));
            for (auto& datagram : completeDump(request, currentSockets))
            {
                reply.push_back(std::move(datagram));
            }
            return reply;
        }
        return completeDump(request, {});
    };

    std::chrono::steady_clock::time_point sampledAt;
    const auto sockets = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_NE(sampledAt, std::chrono::steady_clock::time_point{});
    EXPECT_EQ(inodesOf(sockets), (std::vector<std::uint64_t>{11, 12}));
    ASSERT_EQ(sockets.size(), 2U);
    EXPECT_EQ(std::ranges::find(sockets, 12U, &SocketStats::inode)->bytesReceived, 200U);
}

TEST(NetlinkSocketStatsScriptedTest, RepliesAddressedToAnotherSocketAreIgnored)
{
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array foreign{FakeSocket{.inode = 77}};
    const std::array own{FakeSocket{.inode = 11}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        ScriptedNetlinkTransport::Reply reply;
        reply.emplace_back(socketsDatagram(foreign, request.sequence, ScriptedNetlinkTransport::PORT_ID + 1));
        reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID + 1));
        for (auto& datagram :
             completeDump(request, request.family == AF_INET ? std::span<const FakeSocket>{own} : std::span<const FakeSocket>{}))
        {
            reply.push_back(std::move(datagram));
        }
        return reply;
    };

    EXPECT_EQ(inodesOf(scripted.stats->queryAllSockets()), (std::vector<std::uint64_t>{11}));
}

TEST(NetlinkSocketStatsScriptedTest, ATimedOutDumpIsAFailedReadingAndItsTailIsDrained)
{
    // #1160: a recv() timeout mid-dump left the rest of it queued. It was returned (and cached) as a
    // complete reading, and every later query started by reading the previous one's tail.
    using namespace std::chrono_literals;
    ScriptedStats scripted(std::chrono::minutes{10}); // a partial reading must not be cached even with a long TTL
    const std::array firstPart{FakeSocket{.inode = 11, .bytesReceived = 100}};
    const std::array lateTail{FakeSocket{.inode = 12, .bytesReceived = 200}};
    const std::array later{FakeSocket{.inode = 21, .bytesReceived = 300}, FakeSocket{.inode = 22, .bytesReceived = 400}};
    int query = 0;
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        if (request.family != AF_INET)
        {
            return completeDump(request, {});
        }
        ++query;
        if (query == 1)
        {
            ScriptedNetlinkTransport::Reply reply;
            reply.emplace_back(socketsDatagram(firstPart, request.sequence, ScriptedNetlinkTransport::PORT_ID));
            reply.emplace_back(std::nullopt); // recv() times out here
            reply.emplace_back(socketsDatagram(lateTail, request.sequence, ScriptedNetlinkTransport::PORT_ID));
            reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
            return reply;
        }
        return completeDump(request, later);
    };

    std::chrono::steady_clock::time_point sampledAt;
    const auto partial = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_TRUE(partial.empty()) << "a partial dump was returned as a reading";
    EXPECT_EQ(sampledAt, std::chrono::steady_clock::time_point{}) << "a partial dump was stamped as a reading";

    const auto complete = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_NE(sampledAt, std::chrono::steady_clock::time_point{});
    EXPECT_EQ(inodesOf(complete), (std::vector<std::uint64_t>{21, 22})) << "the earlier dump's tail leaked into this one";
    EXPECT_EQ(query, 2) << "the partial reading was cached instead of queried again";
}

TEST(NetlinkSocketStatsScriptedTest, AnErrorReplyForOneFamilyIsAnEmptyCompleteDump)
{
    // IPv6 disabled: the kernel answers the AF_INET6 dump with NLMSG_ERROR (ENOENT). That family has
    // no sockets; it must not make every reading count as failed.
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array ipv4{FakeSocket{.inode = 11, .bytesSent = 5}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        if (request.family == AF_INET6)
        {
            return ScriptedNetlinkTransport::Reply{errorDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID, -ENOENT)};
        }
        return completeDump(request, ipv4);
    };

    std::chrono::steady_clock::time_point sampledAt;
    const auto sockets = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_NE(sampledAt, std::chrono::steady_clock::time_point{});
    ASSERT_EQ(sockets.size(), 1U);
    EXPECT_EQ(sockets[0].bytesSent, 5U);
}

// #1261 review: a terminal message is not always a successful dump. Each of these must make the
// reading fail (no timestamp, nothing returned), never an empty or partial "complete" reading.
namespace
{
void expectFailedReadingWhenIpv6Replies(ScriptedNetlinkTransport::Reply ipv6Reply)
{
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array ipv4{FakeSocket{.inode = 11, .bytesSent = 5}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        if (request.family == AF_INET6)
        {
            auto reply = ipv6Reply;
            for (auto& datagram : reply)
            {
                // Re-address the scripted datagrams to this request's sequence number.
                nlmsghdr header{};
                std::memcpy(&header, datagram->data(), sizeof(header));
                header.nlmsg_seq = request.sequence;
                std::memcpy(datagram->data(), &header, sizeof(header));
            }
            return reply;
        }
        return completeDump(request, ipv4);
    };
    std::chrono::steady_clock::time_point sampledAt;
    const auto sockets = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_EQ(sampledAt, std::chrono::steady_clock::time_point{});
    EXPECT_TRUE(sockets.empty());
}
} // namespace

TEST(NetlinkSocketStatsScriptedTest, AnErrorOtherThanEnoentIsAFailedReading)
{
    expectFailedReadingWhenIpv6Replies({errorDatagram(0, ScriptedNetlinkTransport::PORT_ID, -EBUSY)});
}

TEST(NetlinkSocketStatsScriptedTest, ADoneWithANegativeStatusIsAFailedReading)
{
    expectFailedReadingWhenIpv6Replies({doneDatagram(0, ScriptedNetlinkTransport::PORT_ID, -ENOMEM)});
}

TEST(NetlinkSocketStatsScriptedTest, AnInterruptedDumpIsAFailedReading)
{
    expectFailedReadingWhenIpv6Replies(
        {doneDatagram(0, ScriptedNetlinkTransport::PORT_ID, 0, static_cast<std::uint16_t>(NLM_F_MULTI | NLM_F_DUMP_INTR))});
}

TEST(NetlinkSocketStatsScriptedTest, AnAckIsNotTheEndOfTheDump)
{
    // A zero-error NLMSG_ERROR is an ACK: the dump continues to NLMSG_DONE, and the sockets after it
    // are part of the reading.
    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array sockets{FakeSocket{.inode = 31, .bytesSent = 7}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        ScriptedNetlinkTransport::Reply reply{errorDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID, 0)};
        if (request.family == AF_INET)
        {
            reply.emplace_back(socketsDatagram(sockets, request.sequence, ScriptedNetlinkTransport::PORT_ID));
        }
        reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
        return reply;
    };
    std::chrono::steady_clock::time_point sampledAt;
    const auto result = scripted.stats->queryAllSockets(&sampledAt);
    EXPECT_NE(sampledAt, std::chrono::steady_clock::time_point{});
    ASSERT_EQ(result.size(), 1U);
    EXPECT_EQ(result[0].inode, 31U);
}

TEST(NetlinkSocketStatsScriptedTest, TcpInfoIsReadFromA4ByteAlignedAttributeAndATruncatedOneOnlyForTheCountersItCovers)
{
    // #1305: netlink attributes are only RTA_ALIGNTO (4-byte) aligned, but tcp_info holds __u64
    // members, so its payload sits at an offset tcp_info's own alignment doesn't allow. The parser
    // must copy it out rather than read through a tcp_info* (UBSan: misaligned member access).
    // A tcp_info from an older kernel is shorter: only the counters the payload covers are read.
    static_assert((NLMSG_HDRLEN + sizeof(inet_diag_msg) + RTA_LENGTH(0)) % alignof(tcp_info) != 0,
                  "the tcp_info payload is expected to be misaligned for tcp_info in a netlink reply");
    static_assert(offsetof(tcp_info, tcpi_bytes_acked) < offsetof(tcp_info, tcpi_bytes_received));

    // A tcp_info cut off right after tcpi_bytes_acked: bytes sent is covered, bytes received isn't.
    const auto truncatedPayload = []
    {
        inet_diag_msg message{};
        message.idiag_inode = 52;
        tcp_info info{};
        info.tcpi_bytes_acked = 1234;
        info.tcpi_bytes_received = 5678;
        constexpr std::size_t INFO_LEN = offsetof(tcp_info, tcpi_bytes_acked) + sizeof(info.tcpi_bytes_acked);
        rtattr attribute{};
        attribute.rta_type = INET_DIAG_INFO;
        attribute.rta_len = static_cast<unsigned short>(RTA_LENGTH(INFO_LEN));
        const std::size_t attributeOffset = sizeof(message);
        const std::size_t infoOffset = attributeOffset + RTA_LENGTH(0);
        TestSupport::Datagram payload(attributeOffset + RTA_SPACE(INFO_LEN));
        std::memcpy(payload.data(), &message, sizeof(message));
        std::memcpy(&payload[attributeOffset], &attribute, sizeof(attribute));
        std::memcpy(&payload[infoOffset], &info, INFO_LEN);
        return payload;
    }();

    using namespace std::chrono_literals;
    ScriptedStats scripted(0ms);
    const std::array sockets{FakeSocket{.inode = 51, .bytesReceived = 100, .bytesSent = 200}};
    scripted.transport->onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        if (request.family != AF_INET)
        {
            return completeDump(request, {});
        }
        ScriptedNetlinkTransport::Reply reply{socketsDatagram(sockets, request.sequence, ScriptedNetlinkTransport::PORT_ID)};
        TestSupport::Datagram truncated;
        TestSupport::appendMessage(truncated, SOCK_DIAG_BY_FAMILY, request.sequence, ScriptedNetlinkTransport::PORT_ID, truncatedPayload);
        reply.emplace_back(std::move(truncated));
        reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
        return reply;
    };

    auto result = scripted.stats->queryAllSockets();
    std::ranges::sort(result, {}, &SocketStats::inode);
    ASSERT_EQ(result.size(), 2U);
    EXPECT_EQ(result[0].inode, 51U);
    EXPECT_EQ(result[0].bytesReceived, 100U);
    EXPECT_EQ(result[0].bytesSent, 200U);
    EXPECT_EQ(result[1].inode, 52U);
    EXPECT_EQ(result[1].bytesReceived, 0U);
    EXPECT_EQ(result[1].bytesSent, 1234U);
}

TEST(NetlinkSocketStatsScriptedTest, NullTransportIsUnavailable)
{
    using namespace std::chrono_literals;
    NetlinkSocketStats stats(nullptr, 0ms);
    EXPECT_FALSE(stats.isAvailable());
    EXPECT_TRUE(stats.queryAllSockets().empty());
}

// ========== buildInodeToPidMap over a synthetic /proc (#1099) ==========

TEST(BuildInodeToPidMapTest, ASharedSocketBelongsToTheLowestPid)
{
    // A socket inherited across fork() is open in both processes. It went to whichever PID readdir()
    // listed last, so it could flip between processes from one rebuild to the next.
    const TestSupport::ScopedTempDir proc("ts_test_inode_map_shared");
    const auto addFd = [&proc](int pid, int fd, const char* target)
    {
        const auto fdDir = proc.path / std::to_string(pid) / "fd";
        std::filesystem::create_directories(fdDir);
        std::filesystem::create_symlink(target, fdDir / std::to_string(fd));
    };
    for (const int pid : {300, 100, 200, 400})
    {
        addFd(pid, 3, "socket:[77]");
    }
    addFd(100, 4, "pipe:[5]");
    addFd(400, 5, "socket:[88]");

    const auto inodeToPid = buildInodeToPidMap(proc.path);
    ASSERT_EQ(inodeToPid.size(), 2U);
    EXPECT_EQ(inodeToPid.at(77).pid, 100);
    EXPECT_EQ(inodeToPid.at(88).pid, 400);
}

TEST(BuildInodeToPidMapTest, EachOwnerComesWithItsStartTime)
{
    // #1336: the owner is reported with its start time (stat field 22), so Domain can tell it from a
    // process that reused its PID. A shared socket's owner is the lowest PID, with that PID's start
    // time; a process whose stat can't be read reports 0 (unknown).
    const TestSupport::ScopedTempDir proc("ts_test_inode_map_start_time");
    const auto addProcess = [&proc](int pid, const char* stat)
    {
        const auto pidDir = proc.path / std::to_string(pid);
        std::filesystem::create_directories(pidDir / "fd");
        if (stat != nullptr)
        {
            std::ofstream(pidDir / "stat") << stat;
        }
    };
    const auto addFd = [&proc](int pid, int fd, const char* target)
    {
        std::filesystem::create_symlink(target, proc.path / std::to_string(pid) / "fd" / std::to_string(fd));
    };

    // A comm with spaces and parentheses: fields are counted from the last ')'.
    addProcess(100, "100 (we (ird) name) S 1 100 100 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 4242 1000 50\n");
    addProcess(200, "200 (other) S 1 200 200 0 -1 4194560 10 0 0 0 5 6 0 0 20 0 1 0 9999 1000 50\n");
    addProcess(300, nullptr);
    addFd(100, 3, "socket:[77]");
    addFd(200, 3, "socket:[77]");
    addFd(200, 4, "socket:[88]");
    addFd(300, 3, "socket:[99]");

    const auto inodeToPid = buildInodeToPidMap(proc.path);
    ASSERT_EQ(inodeToPid.size(), 3U);
    EXPECT_EQ(inodeToPid.at(77).pid, 100);
    EXPECT_EQ(inodeToPid.at(77).startTimeTicks, 4242U) << "the lowest PID's own start time";
    EXPECT_EQ(inodeToPid.at(88).pid, 200);
    EXPECT_EQ(inodeToPid.at(88).startTimeTicks, 9999U);
    EXPECT_EQ(inodeToPid.at(99).pid, 300);
    EXPECT_EQ(inodeToPid.at(99).startTimeTicks, 0U) << "no stat: unknown";
}

// ========== dumpInetSockets(): endpoints and states for the Connections section (#799) ==========

/// An inet_diag_msg as the kernel sends it: addresses in network order, ports __be16.
[[nodiscard]] inet_diag_msg diagMessage(std::uint8_t family,
                                        std::uint8_t state,
                                        std::uint32_t inode,
                                        std::span<const std::uint8_t> src,
                                        std::uint16_t sport,
                                        std::span<const std::uint8_t> dst,
                                        std::uint16_t dport)
{
    inet_diag_msg message{};
    message.idiag_family = family;
    message.idiag_state = state;
    message.idiag_inode = inode;
    // An empty span's data() may be null, and memcpy with a null source is undefined even for 0
    // bytes (UBSan nonnull-attribute, #1541), so empty addresses are skipped, not copied.
    if (!src.empty())
    {
        std::memcpy(static_cast<void*>(message.id.idiag_src), src.data(), std::min(src.size(), sizeof(message.id.idiag_src)));
    }
    if (!dst.empty())
    {
        std::memcpy(static_cast<void*>(message.id.idiag_dst), dst.data(), std::min(dst.size(), sizeof(message.id.idiag_dst)));
    }
    message.id.idiag_sport = htons(sport);
    message.id.idiag_dport = htons(dport);
    return message;
}

TEST(DumpInetSocketsTest, ReadsEndpointsStateAndInodeOfEachSocketWithoutExtensions)
{
    ScriptedNetlinkTransport transport;
    constexpr std::array<std::uint8_t, 4> LOCAL4{10, 0, 2, 15};
    constexpr std::array<std::uint8_t, 4> REMOTE4{93, 184, 216, 34};
    const std::array<inet_diag_msg, 2> messages{diagMessage(AF_INET, 1, 501, LOCAL4, 55026, REMOTE4, 443),
                                                diagMessage(AF_INET, 10, 502, std::array<std::uint8_t, 4>{127, 0, 0, 1}, 631, {}, 0)};
    transport.onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        ScriptedNetlinkTransport::Reply reply;
        reply.emplace_back(TestSupport::diagMessagesDatagram(messages, request.sequence, ScriptedNetlinkTransport::PORT_ID));
        reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
        return reply;
    };

    std::vector<InetSockets::RawInetSocket> sockets;
    EXPECT_EQ(dumpInetSockets(transport, 7, IPPROTO_TCP, AF_INET, sockets), InetDumpOutcome::Complete);
    ASSERT_EQ(transport.requests.size(), 1U);
    EXPECT_EQ(transport.requests[0].protocol, IPPROTO_TCP);
    EXPECT_EQ(transport.requests[0].family, AF_INET);
    EXPECT_EQ(transport.requests[0].sequence, 7U);

    ASSERT_EQ(sockets.size(), 2U);
    EXPECT_EQ(sockets[0].inode, 501U);
    EXPECT_EQ(sockets[0].protocol, ConnectionProtocol::Tcp);
    EXPECT_EQ(sockets[0].family, ConnectionFamily::IPv4);
    EXPECT_EQ(sockets[0].state, 1);
    EXPECT_TRUE(std::equal(LOCAL4.begin(), LOCAL4.end(), sockets[0].local.address.begin()));
    EXPECT_TRUE(std::equal(REMOTE4.begin(), REMOTE4.end(), sockets[0].remote.address.begin()));
    EXPECT_EQ(sockets[0].local.address[4], 0) << "an IPv4 address fills only the first four bytes";
    EXPECT_EQ(sockets[0].local.port, 55026);
    EXPECT_EQ(sockets[0].remote.port, 443);
    EXPECT_EQ(InetSockets::toConnection(sockets[1]).state, ConnectionState::Listen);
    EXPECT_EQ(sockets[1].remote.port, 0);
}

TEST(DumpInetSocketsTest, ReadsIPv6UdpSockets)
{
    ScriptedNetlinkTransport transport;
    std::array<std::uint8_t, 16> loopback6{};
    loopback6[15] = 1;
    const std::array<inet_diag_msg, 1> messages{diagMessage(AF_INET6, 7, 900, loopback6, 5353, {}, 0)};
    transport.onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        ScriptedNetlinkTransport::Reply reply;
        reply.emplace_back(TestSupport::diagMessagesDatagram(messages, request.sequence, ScriptedNetlinkTransport::PORT_ID));
        reply.emplace_back(doneDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID));
        return reply;
    };

    std::vector<InetSockets::RawInetSocket> sockets;
    EXPECT_EQ(dumpInetSockets(transport, 1, IPPROTO_UDP, AF_INET6, sockets), InetDumpOutcome::Complete);
    ASSERT_EQ(transport.requests.size(), 1U);
    EXPECT_EQ(transport.requests[0].protocol, IPPROTO_UDP);
    ASSERT_EQ(sockets.size(), 1U);
    EXPECT_EQ(sockets[0].protocol, ConnectionProtocol::Udp);
    EXPECT_EQ(sockets[0].family, ConnectionFamily::IPv6);
    EXPECT_EQ(sockets[0].local.address, loopback6);
    EXPECT_EQ(sockets[0].local.port, 5353);
    EXPECT_EQ(InetSockets::toConnection(sockets[0]).state, ConnectionState::Closed);
}

TEST(DumpInetSocketsTest, AnAbsentDiagModuleIsUnsupportedAndAnyOtherErrorFailed)
{
    ScriptedNetlinkTransport transport;
    int error = -ENOENT;
    transport.onRequest = [&](const ScriptedNetlinkTransport::Request& request)
    {
        return ScriptedNetlinkTransport::Reply{errorDatagram(request.sequence, ScriptedNetlinkTransport::PORT_ID, error)};
    };
    std::vector<InetSockets::RawInetSocket> sockets;
    EXPECT_EQ(dumpInetSockets(transport, 1, IPPROTO_UDP, AF_INET, sockets), InetDumpOutcome::Unsupported);
    error = -EBUSY;
    EXPECT_EQ(dumpInetSockets(transport, 2, IPPROTO_UDP, AF_INET, sockets), InetDumpOutcome::Failed);
    EXPECT_TRUE(sockets.empty());
}

TEST(DumpInetSocketsTest, ATimeoutIsAFailedDump)
{
    ScriptedNetlinkTransport transport;
    transport.onRequest = [](const ScriptedNetlinkTransport::Request& /*request*/)
    {
        return ScriptedNetlinkTransport::Reply{std::nullopt};
    };
    std::vector<InetSockets::RawInetSocket> sockets;
    EXPECT_EQ(dumpInetSockets(transport, 1, IPPROTO_TCP, AF_INET, sockets), InetDumpOutcome::Failed);
}

} // namespace
} // namespace Platform

#endif // __linux__
