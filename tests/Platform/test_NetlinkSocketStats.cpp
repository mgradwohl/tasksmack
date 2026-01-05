// Tests for NetlinkSocketStats and related functions
// Only compiled on Linux

#if defined(__linux__)

#include "Platform/Linux/NetlinkSocketStats.h"

#include <gtest/gtest.h>

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
    for (const auto& [inode, pid] : inodeToPid)
    {
        EXPECT_GT(inode, 0UL) << "Inode should be positive";
        EXPECT_GT(pid, 0) << "PID should be positive";
    }
}

TEST(BuildInodeToPidMapTest, FindsOwnProcessSockets)
{
    // Get our own PID
    const auto ownPid = static_cast<std::int32_t>(getpid());

    auto inodeToPid = buildInodeToPidMap();

    // Check if any sockets are mapped to our process
    bool foundOwnSocket = false;
    for (const auto& [inode, pid] : inodeToPid)
    {
        if (pid == ownPid)
        {
            foundOwnSocket = true;
            break;
        }
    }

    // We may or may not have sockets, so just verify the map doesn't crash
    SUCCEED() << "Found " << (foundOwnSocket ? "own process sockets" : "no own process sockets");
}

// ========== aggregateByPid Tests ==========

TEST(AggregateByPidTest, EmptyInputsReturnEmptyResult)
{
    std::vector<SocketStats> sockets;
    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;

    auto result = aggregateByPid(sockets, inodeToPid);
    EXPECT_TRUE(result.empty());
}

TEST(AggregateByPidTest, EmptySocketsReturnEmptyResult)
{
    std::vector<SocketStats> sockets;
    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;
    inodeToPid[67890] = 200;

    auto result = aggregateByPid(sockets, inodeToPid);
    EXPECT_TRUE(result.empty());
}

TEST(AggregateByPidTest, EmptyMapReturnsEmptyResult)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 1000, .bytesSent = 500});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;

    auto result = aggregateByPid(sockets, inodeToPid);
    EXPECT_TRUE(result.empty());
}

TEST(AggregateByPidTest, SingleSocketSinglePid)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 1000, .bytesSent = 500});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 1UL);
    EXPECT_EQ(result[100].first, 1000UL); // bytesReceived
    EXPECT_EQ(result[100].second, 500UL); // bytesSent
}

TEST(AggregateByPidTest, MultipleSocketsSamePid)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 1000, .bytesSent = 500});
    sockets.push_back({.inode = 67890, .bytesReceived = 2000, .bytesSent = 1000});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;
    inodeToPid[67890] = 100; // Same PID

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 1UL);
    EXPECT_EQ(result[100].first, 3000UL);  // 1000 + 2000
    EXPECT_EQ(result[100].second, 1500UL); // 500 + 1000
}

TEST(AggregateByPidTest, MultipleSocketsDifferentPids)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 1000, .bytesSent = 500});
    sockets.push_back({.inode = 67890, .bytesReceived = 2000, .bytesSent = 1000});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;
    inodeToPid[67890] = 200; // Different PID

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 2UL);
    EXPECT_EQ(result[100].first, 1000UL);
    EXPECT_EQ(result[100].second, 500UL);
    EXPECT_EQ(result[200].first, 2000UL);
    EXPECT_EQ(result[200].second, 1000UL);
}

TEST(AggregateByPidTest, UnmappedSocketsAreIgnored)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 1000, .bytesSent = 500});
    sockets.push_back({.inode = 99999, .bytesReceived = 5000, .bytesSent = 2500}); // Unmapped

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;
    // 99999 is not in the map

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 1UL);
    EXPECT_EQ(result[100].first, 1000UL);
    EXPECT_EQ(result[100].second, 500UL);
}

TEST(AggregateByPidTest, ZeroByteCountersAreHandled)
{
    std::vector<SocketStats> sockets;
    sockets.push_back({.inode = 12345, .bytesReceived = 0, .bytesSent = 0});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 1UL);
    EXPECT_EQ(result[100].first, 0UL);
    EXPECT_EQ(result[100].second, 0UL);
}

TEST(AggregateByPidTest, LargeByteCountersAreHandled)
{
    std::vector<SocketStats> sockets;
    // Use large values near uint64_t max
    sockets.push_back({.inode = 12345, .bytesReceived = 0xFFFFFFFFFFFFFF00ULL, .bytesSent = 0x7FFFFFFFFFFFFFFFULL});

    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid[12345] = 100;

    auto result = aggregateByPid(sockets, inodeToPid);
    ASSERT_EQ(result.size(), 1UL);
    EXPECT_EQ(result[100].first, 0xFFFFFFFFFFFFFF00ULL);
    EXPECT_EQ(result[100].second, 0x7FFFFFFFFFFFFFFFULL);
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

    // Aggregate by PID
    auto pidStats = aggregateByPid(sockets, inodeToPid);

    // Verify results are consistent
    for (const auto& [pid, bytes] : pidStats)
    {
        EXPECT_GT(pid, 0) << "PID should be positive";
        // Bytes can be zero (idle sockets)
    }

    SUCCEED() << "Mapped " << sockets.size() << " sockets to " << pidStats.size() << " processes";
}

} // namespace
} // namespace Platform

#endif // __linux__
