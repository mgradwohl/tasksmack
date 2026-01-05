#pragma once

// Only compile on Linux with required headers
#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Platform
{

/// Per-socket network statistics from Netlink INET_DIAG
struct SocketStats
{
    std::uint64_t inode = 0;         // Socket inode (for PID mapping)
    std::uint64_t bytesReceived = 0; // Cumulative bytes received
    std::uint64_t bytesSent = 0;     // Cumulative bytes sent
};

/// Queries TCP/UDP socket statistics via Netlink INET_DIAG.
/// This provides per-socket byte counters that can be mapped to processes.
class NetlinkSocketStats
{
  public:
    NetlinkSocketStats();
    ~NetlinkSocketStats();

    NetlinkSocketStats(const NetlinkSocketStats&) = delete;
    NetlinkSocketStats& operator=(const NetlinkSocketStats&) = delete;
    NetlinkSocketStats(NetlinkSocketStats&&) = delete;
    NetlinkSocketStats& operator=(NetlinkSocketStats&&) = delete;

    /// Query all TCP and UDP sockets with byte counters.
    /// Returns a vector of SocketStats with inode and byte counters.
    [[nodiscard]] std::vector<SocketStats> queryAllSockets();

    /// Check if Netlink INET_DIAG is available and functional
    [[nodiscard]] bool isAvailable() const
    {
        return m_Available;
    }

  private:
    int m_Socket = -1;                // Netlink socket file descriptor
    bool m_Available = false;         // Whether INET_DIAG is functional
    mutable std::mutex m_SocketMutex; // Protects socket operations for thread safety

    /// Query sockets for a specific protocol (IPPROTO_TCP or IPPROTO_UDP)
    void querySockets(int protocol, std::vector<SocketStats>& results);

    /// Parse a single inet_diag_msg response
    static void parseSocketMessage(const void* msg, std::size_t len, std::vector<SocketStats>& results);
};

/// Build a mapping from socket inode to owning PID by scanning /proc/[pid]/fd/*
/// Returns map: inode -> PID
[[nodiscard]] std::unordered_map<std::uint64_t, std::int32_t> buildInodeToPidMap();

/// Aggregate socket stats by PID using the inode-to-PID mapping.
/// Returns map: PID -> (totalBytesReceived, totalBytesSent)
[[nodiscard]] std::unordered_map<std::int32_t, std::pair<std::uint64_t, std::uint64_t>>
aggregateByPid(const std::vector<SocketStats>& sockets, const std::unordered_map<std::uint64_t, std::int32_t>& inodeToPid);

} // namespace Platform

#endif // __linux__ && headers available
