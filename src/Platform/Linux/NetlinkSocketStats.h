#pragma once

// Only compile on Linux with required headers
#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace Platform
{

/// Default TTL for socket stats cache (500ms balances freshness vs. CPU cost)
/// Network stats don't need to be as fresh as CPU/memory metrics.
///
/// This default can be overridden at runtime via LinuxProcessProbe::setSocketStatsCacheTtl()
/// or via user config [sampling] socket_stats_cache_ttl_ms setting.
/// See Domain/SamplingConfig.h SOCKET_STATS_CACHE_TTL_MS_* constants.
inline constexpr auto DEFAULT_SOCKET_STATS_CACHE_TTL = std::chrono::milliseconds(500);

/// Per-socket network statistics from Netlink INET_DIAG
struct SocketStats
{
    std::uint64_t inode = 0;         // Socket inode (for PID mapping)
    std::uint64_t bytesReceived = 0; // Cumulative bytes received
    std::uint64_t bytesSent = 0;     // Cumulative bytes sent
};

/// Result of one send(2)/recv(2) on a netlink socket: bytes transferred, or -1 with `error` set
/// to the errno (EAGAIN/EWOULDBLOCK when a receive timed out or, non-blocking, nothing was queued).
struct NetlinkIoResult
{
    std::int64_t bytes = -1;
    int error = 0;
};

/// The NETLINK_SOCK_DIAG socket NetlinkSocketStats talks to. A seam so tests can script the
/// kernel's replies (stale tails, timeouts) and see the requests sent.
class INetlinkTransport
{
  public:
    INetlinkTransport() = default;
    virtual ~INetlinkTransport() = default;
    INetlinkTransport(const INetlinkTransport&) = delete;
    INetlinkTransport& operator=(const INetlinkTransport&) = delete;
    INetlinkTransport(INetlinkTransport&&) = delete;
    INetlinkTransport& operator=(INetlinkTransport&&) = delete;

    /// Send one request message.
    [[nodiscard]] virtual NetlinkIoResult send(std::span<const std::byte> request) = 0;
    /// Receive one datagram. Blocking reads are bounded by the socket's receive timeout;
    /// `nonBlocking` returns at once when nothing is queued (used to drain stale replies).
    [[nodiscard]] virtual NetlinkIoResult receive(std::span<std::byte> buffer, bool nonBlocking) = 0;
    /// The port ID the kernel addresses this socket's replies to (their nlmsg_pid); 0 = unknown.
    [[nodiscard]] virtual std::uint32_t portId() const noexcept = 0;
};

/// Queries TCP socket statistics via Netlink INET_DIAG.
/// This provides per-socket byte counters that can be mapped to processes.
///
/// TCP only (#1101): the kernel reports byte counters (tcp_info's tcpi_bytes_received and
/// tcpi_bytes_acked, Linux 4.2+) only for TCP. A UDP sock_diag dump carries no INET_DIAG_INFO and
/// no byte counts at all, so UDP traffic -- QUIC/HTTP3, WebRTC calls, games, DNS -- can't be
/// attributed to a process this way, and UDP sockets are not queried. Attributing it would need
/// eBPF or per-cgroup accounting.
///
/// Each dump request carries a fresh sequence number, and the reader keeps only replies with that
/// sequence number addressed to this socket, after draining anything still queued from an earlier
/// dump; a dump that times out or fails mid-way is reported as failed rather than returned or
/// cached as a complete reading (#1160).
///
/// Performance optimization: Results are cached with a configurable TTL to avoid
/// expensive kernel queries on every call. The default TTL of 500ms balances
/// network stat freshness against CPU cost (~10% of refresh cycle without caching).
class NetlinkSocketStats
{
  public:
    /// Construct with default cache TTL (500ms)
    NetlinkSocketStats();

    /// Construct with custom cache TTL
    /// @param cacheTtl Time-to-live for cached results. Use 0ms to disable caching.
    explicit NetlinkSocketStats(std::chrono::milliseconds cacheTtl);

    /// Test seam: query through `transport` instead of a real NETLINK_SOCK_DIAG socket.
    /// A null transport leaves the instance unavailable.
    NetlinkSocketStats(std::unique_ptr<INetlinkTransport> transport, std::chrono::milliseconds cacheTtl);

    ~NetlinkSocketStats() noexcept;

    NetlinkSocketStats(const NetlinkSocketStats&) = delete;
    NetlinkSocketStats& operator=(const NetlinkSocketStats&) = delete;
    NetlinkSocketStats(NetlinkSocketStats&&) = delete;
    NetlinkSocketStats& operator=(NetlinkSocketStats&&) = delete;

    /// Query all TCP sockets with byte counters.
    /// Returns a vector of SocketStats with inode and byte counters.
    /// Results are cached; subsequent calls within the TTL return cached data.
    /// A failed or incomplete kernel dump returns an empty vector, leaves `*sampledAt` untouched,
    /// and is not cached, so a caller can tell it from a complete reading with no sockets.
    /// @param sampledAt If non-null, receives when the returned data was read from the kernel
    ///                  (the original query time for a cache hit).
    [[nodiscard]] std::vector<SocketStats> queryAllSockets(std::chrono::steady_clock::time_point* sampledAt = nullptr);

    /// Force a fresh kernel query, completely bypassing the cache.
    /// This does NOT update the internal cache; subsequent queryAllSockets() calls
    /// will still use the existing cached data until TTL expires. Empty if the dump failed.
    [[nodiscard]] std::vector<SocketStats> queryAllSocketsUncached();

    /// Check if Netlink INET_DIAG is available and functional
    [[nodiscard]] bool isAvailable() const noexcept
    {
        return m_Available;
    }

    /// Get the configured cache TTL
    [[nodiscard]] std::chrono::milliseconds cacheTtl() const noexcept
    {
        return m_CacheTtl;
    }

    /// Invalidate the cache (next query will hit the kernel)
    void invalidateCache() noexcept;

  private:
    std::unique_ptr<INetlinkTransport> m_Transport; // Netlink socket (null when unavailable)
    bool m_Available = false;                       // Whether INET_DIAG is functional
    mutable std::mutex m_SocketMutex;               // Protects socket operations and cache state for thread safety
    std::uint32_t m_NextSequence = 1;               // nlmsg_seq of the next dump request (guarded by m_SocketMutex)

    // Cache state
    std::chrono::milliseconds m_CacheTtl;                   // Cache time-to-live
    std::chrono::steady_clock::time_point m_LastQueryTime;  // When cache was last populated
    std::vector<SocketStats> m_CachedResults;               // Cached socket stats
    std::chrono::steady_clock::time_point m_LastSampleTime; // When the last kernel query ran (even with TTL 0)

    /// Dump every TCP socket (IPv4 and IPv6) into `results`. False if a dump timed out or failed
    /// mid-way (`results` then holds a partial reading the caller must not use).
    [[nodiscard]] bool queryTcpSockets(std::vector<SocketStats>& results);

    /// One INET_DIAG dump for `protocol` and address `family`; see queryTcpSockets().
    [[nodiscard]] bool queryDump(int protocol, int family, std::vector<SocketStats>& results);

    /// Parse a single inet_diag_msg response
    static void parseSocketMessage(const void* msg, std::size_t len, std::vector<SocketStats>& results);
};

/// Build a mapping from socket inode to owning PID by scanning <procRoot>/[pid]/fd/*
/// Returns map: inode -> PID. A socket open in several processes (inherited across fork(), passed
/// over a UNIX socket) maps to the lowest PID, so its owner doesn't flip between rebuilds with
/// readdir() order (#1099).
[[nodiscard]] std::unordered_map<std::uint64_t, std::int32_t> buildInodeToPidMap(const std::filesystem::path& procRoot = "/proc");

} // namespace Platform

#endif // __linux__ && headers available
