// Only compile on Linux with required headers
#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include "NetlinkSocketStats.h"

#include "InetSocketTable.h"
#include "Platform/IProcessConnections.h"
#include "PosixGuards.h"
#include "ProcFdScan.h"
#include "ProcParsing.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX/Linux headers: include-cleaner lacks mappings for ssize_t, strerror_r, IPPROTO_*
#include <dirent.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform
{

namespace
{

// Buffer size for netlink messages (should be large enough for typical response)
constexpr std::size_t NETLINK_BUFFER_SIZE = 65536;

// Bounds how long querySocketsForFamily() can block in recv() on the background sampler
// thread. Without this, a kernel dump that never emits its multipart terminator (memory
// pressure, a namespace/container quirk) would hang the sampler thread indefinitely.
constexpr long NETLINK_RECV_TIMEOUT_MS = 2000;

/// Thread-safe strerror wrapper using strerror_r
/// Handles both GNU (returns char*) and POSIX (returns int) versions using compile-time detection
[[nodiscard]] std::string safeStrerror(int errnum)
{
    std::array<char, 256> buffer{};

    // NOLINTNEXTLINE(concurrency-mt-unsafe,misc-include-cleaner) - using thread-safe strerror_r, include-cleaner false positive for POSIX
    auto* result = strerror_r(errnum, buffer.data(), buffer.size());

    // Handle both GNU (returns char*) and POSIX (returns int) variants
    if constexpr (std::is_same_v<decltype(result), char*>)
    {
        // GNU variant: returns char* (may return static string or use buffer)
        if (result != nullptr)
        {
            return {result};
        }
    }
    else if constexpr (std::is_same_v<decltype(result), int>)
    {
        // POSIX/XSI variant: returns int, string is in buffer
        // NOLINTNEXTLINE(modernize-use-nullptr) - POSIX variant: result is int, not pointer
        if (result == 0)
        {
            return {buffer.data()};
        }
    }

    return std::format("Unknown error {}", errnum);
}

// Request structure for inet_diag with extensions
// Note: nlmsghdr and inet_diag_req_v2 are kernel structures with well-defined layouts.
// The kernel netlink protocol guarantees proper alignment and packing for these structures
// when used contiguously, so no explicit packing attribute is needed.
struct InetDiagRequest
{
    nlmsghdr nlh;
    inet_diag_req_v2 req;
};

// Static assertion to verify the struct layout matches expectations
// The netlink header is 16 bytes (4 x __u32) and inet_diag_req_v2 is 56 bytes
static_assert(sizeof(InetDiagRequest) == sizeof(nlmsghdr) + sizeof(inet_diag_req_v2),
              "InetDiagRequest must be tightly packed for netlink protocol");

/// Parse rtattr chain following inet_diag_msg to extract tcp_info byte counters
void parseTcpInfo(const inet_diag_msg* diagMsg, std::size_t msgLen, SocketStats& stats)
{
    // Defensive check: ensure msgLen is at least sizeof(inet_diag_msg) before subtraction
    // to avoid underflow (e.g., from truncated/malformed netlink messages)
    if (msgLen < sizeof(inet_diag_msg))
    {
        return;
    }

    // Calculate where attributes start (after the inet_diag_msg)
    const auto* attrStart = reinterpret_cast<const std::uint8_t*>(diagMsg + 1);
    std::size_t attrLen = (msgLen - sizeof(inet_diag_msg));

    // Ensure we have room for attributes
    if (attrLen < sizeof(rtattr))
    {
        return;
    }

    // Walk through the attributes
    // Note: Suppress the alignment warning for the rtattr casts only - the kernel netlink
    // macros use char* internally and attributes are RTA_ALIGNTO (4-byte) aligned, which
    // satisfies rtattr's 2-byte alignment. Payloads with stricter alignment (tcp_info) are
    // copied out with memcpy rather than accessed in place.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
    for (const auto* rta = reinterpret_cast<const rtattr*>(attrStart); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen))
    {
        if (rta->rta_type == INET_DIAG_INFO)
        {
            // This attribute contains a tcp_info structure. Netlink attributes are only
            // RTA_ALIGNTO (4-byte) aligned, but tcp_info has __u64 members that need 8-byte
            // alignment, so reading through a tcp_info* into the reply buffer is a misaligned
            // access (UB). Copy the payload prefix the kernel sent (bounded by RTA_PAYLOAD and
            // sizeof(tcp_info)) into an aligned, zeroed local and read the fields from that.
            const std::size_t infoLen = RTA_PAYLOAD(rta);
            tcp_info tcpInfo{};
            std::memcpy(&tcpInfo, RTA_DATA(rta), std::min(infoLen, sizeof(tcpInfo)));

            // Check we have enough data for the byte counter fields
            // bytes_acked and bytes_received were added in Linux 4.2
            // They're at offset ~144 bytes into tcp_info
            if (infoLen >= (offsetof(tcp_info, tcpi_bytes_received) + sizeof(tcpInfo.tcpi_bytes_received)))
            {
                stats.bytesReceived = tcpInfo.tcpi_bytes_received;
            }
            if (infoLen >= (offsetof(tcp_info, tcpi_bytes_acked) + sizeof(tcpInfo.tcpi_bytes_acked)))
            {
                stats.bytesSent = tcpInfo.tcpi_bytes_acked;
            }
            break;
        }
    }
#pragma clang diagnostic pop
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

/// Parse a single inet_diag_msg response into SocketStats
void parseSocketMessageImpl(const void* msg, std::size_t len, std::vector<SocketStats>& results)
{
    if (len < sizeof(inet_diag_msg))
    {
        return;
    }

    const auto* diagMsg = static_cast<const inet_diag_msg*>(msg);

    SocketStats stats;
    stats.inode = diagMsg->idiag_inode;

    // Parse tcp_info from the INET_DIAG_INFO attribute to get byte counters
    parseTcpInfo(diagMsg, len, stats);

    if (stats.inode != 0)
    {
        results.push_back(stats);
    }
}

/// The real NETLINK_SOCK_DIAG socket. Owns the fd; isOpen() is false if it couldn't be created or bound.
class SockDiagTransport final : public INetlinkTransport
{
  public:
    SockDiagTransport() : m_Socket(socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG))
    {
        if (m_Socket < 0)
        {
            spdlog::debug("Failed to create NETLINK_SOCK_DIAG socket: {}", safeStrerror(errno));
            return;
        }

        // Bound recv() so a stalled kernel dump can't hang the background sampler thread (or a
        // Connections read, #799) forever (see NetlinkSocketStats::queryDump()). Fails closed: a
        // socket whose receives could block indefinitely is not used at all, and its callers fall
        // back as they do without netlink.
        timeval recvTimeout{}; // NOLINT(misc-include-cleaner) - provided by <sys/time.h> (already included)
        recvTimeout.tv_sec = NETLINK_RECV_TIMEOUT_MS / 1000;
        recvTimeout.tv_usec = (NETLINK_RECV_TIMEOUT_MS % 1000) * 1000;
        // NOLINTNEXTLINE(misc-include-cleaner) - SOL_SOCKET/SO_RCVTIMEO are provided by <sys/socket.h> (already included)
        if (setsockopt(m_Socket, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout)) < 0)
        {
            spdlog::debug("Failed to set SO_RCVTIMEO on netlink socket, not using it: {}", safeStrerror(errno));
            closeSocket();
            return;
        }

        // Bind the socket
        sockaddr_nl addr{};
        addr.nl_family = AF_NETLINK;
        addr.nl_pid = 0;    // Let kernel assign PID
        addr.nl_groups = 0; // No multicast groups

        if (bind(m_Socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            spdlog::debug("Failed to bind netlink socket: {}", safeStrerror(errno));
            closeSocket();
            return;
        }

        // The kernel addresses dump replies to the port ID it assigned at bind(); read it back so
        // the reader can ignore anything not meant for this socket. Unknown (0) disables that check.
        sockaddr_nl bound{};
        socklen_t boundLen = sizeof(bound); // NOLINT(misc-include-cleaner) - socklen_t from <sys/socket.h>
        if (getsockname(m_Socket, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        {
            m_PortId = bound.nl_pid;
        }
    }

    ~SockDiagTransport() noexcept override
    {
        closeSocket();
    }

    SockDiagTransport(const SockDiagTransport&) = delete;
    SockDiagTransport& operator=(const SockDiagTransport&) = delete;
    SockDiagTransport(SockDiagTransport&&) = delete;
    SockDiagTransport& operator=(SockDiagTransport&&) = delete;

    [[nodiscard]] bool isOpen() const noexcept
    {
        return m_Socket >= 0;
    }

    [[nodiscard]] NetlinkIoResult send(std::span<const std::byte> request) override
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - ssize_t is POSIX (<sys/types.h> via <sys/socket.h>)
        const ssize_t sent = ::send(m_Socket, request.data(), request.size(), 0);
        return {.bytes = sent, .error = sent < 0 ? errno : 0};
    }

    [[nodiscard]] NetlinkIoResult receive(std::span<std::byte> buffer, bool nonBlocking) override
    {
        // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection,misc-include-cleaner) - bounded by SO_RCVTIMEO; ssize_t POSIX
        const ssize_t len = ::recv(m_Socket, buffer.data(), buffer.size(), nonBlocking ? MSG_DONTWAIT : 0);
        return {.bytes = len, .error = len < 0 ? errno : 0};
    }

    [[nodiscard]] std::uint32_t portId() const noexcept override
    {
        return m_PortId;
    }

  private:
    void closeSocket() noexcept
    {
        if (m_Socket >= 0)
        {
            // Invalidate m_Socket before close(); close() errors are ignored (common POSIX pattern).
            const int oldSocket = m_Socket;
            m_Socket = -1;
            close(oldSocket);
        }
    }

    int m_Socket = -1;
    std::uint32_t m_PortId = 0;
};

[[nodiscard]] bool isWouldBlock(int error) noexcept
{
    return error == EAGAIN || error == EWOULDBLOCK;
}

/// Discard whatever is still queued on the socket -- the tail of an earlier dump that timed out or
/// was abandoned -- so the next dump's reader doesn't start on it (#1160). Bounded so a socket that
/// keeps delivering can't hold the sampler thread.
void drainQueuedReplies(INetlinkTransport& transport, std::span<std::byte> buffer)
{
    constexpr int MAX_DRAINED_DATAGRAMS = 1024;
    for (int drained = 0; drained < MAX_DRAINED_DATAGRAMS; ++drained)
    {
        const NetlinkIoResult result = transport.receive(buffer, true);
        if (result.bytes < 0 && result.error == EINTR)
        {
            continue;
        }
        if (result.bytes <= 0)
        {
            return; // EAGAIN: nothing left; anything else: the next blocking read reports it
        }
    }
}

/// One INET_DIAG dump of `protocol` sockets of address `family` over `transport`, request `sequence`,
/// asking for the attribute extensions in the `extensions` bitmask; `onSocket(message, length)` is
/// called with each SOCK_DIAG_BY_FAMILY payload. Anything still queued from an earlier dump is
/// drained first. Shared by NetlinkSocketStats' byte-counter dumps and dumpInetSockets() (#799).
template<std::invocable<const void*, std::size_t> OnSocket>
[[nodiscard]] InetDumpOutcome runInetDiagDump(INetlinkTransport& transport,
                                              std::span<std::byte> buffer,
                                              std::uint32_t sequence,
                                              int protocol,
                                              int family,
                                              std::uint8_t extensions,
                                              OnSocket onSocket)
{
    // Anything still queued belongs to an earlier dump (one that timed out, or whose reader gave up
    // on an error): drop it before asking for a new one (#1160).
    drainQueuedReplies(transport, buffer);

    InetDiagRequest req{};
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = sequence;
    req.req.sdiag_family = static_cast<std::uint8_t>(family);
    req.req.sdiag_protocol = static_cast<std::uint8_t>(protocol);
    req.req.idiag_states = static_cast<std::uint32_t>(-1); // All states
    req.req.idiag_ext = extensions;

    const NetlinkIoResult sent = transport.send(std::as_bytes(std::span{&req, 1}));
    if (sent.bytes < 0)
    {
        spdlog::debug("Failed to send inet_diag request for family {}: {}", family, safeStrerror(sent.error));
        return InetDumpOutcome::Failed;
    }

    const std::uint32_t portId = transport.portId();
    while (true)
    {
        const NetlinkIoResult received = transport.receive(buffer, false);
        if (received.bytes < 0)
        {
            if (received.error == EINTR)
            {
                continue;
            }
            // A timeout (EAGAIN) leaves the rest of the dump queued; the next query drains it.
            spdlog::debug("{} inet_diag response for family {}: {}",
                          isWouldBlock(received.error) ? "Timed out waiting for" : "Failed to receive",
                          family,
                          safeStrerror(received.error));
            return InetDumpOutcome::Failed;
        }
        if (received.bytes == 0)
        {
            // Orderly shutdown before NLMSG_DONE: the dump is incomplete.
            return InetDumpOutcome::Failed;
        }

        // Parse netlink messages
        // Suppress alignment warning - the caller's buffer is aligned for nlmsghdr and kernel
        // netlink protocol guarantees proper alignment of messages
        // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
        auto remainingLen = static_cast<std::size_t>(received.bytes);
        for (auto* nlh = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(nlh, remainingLen); nlh = NLMSG_NEXT(nlh, remainingLen))
        {
            // Not a reply to this request: a stale message from an earlier dump, or not addressed
            // to this socket.
            if (nlh->nlmsg_seq != sequence || (portId != 0 && nlh->nlmsg_pid != portId))
            {
                continue;
            }

            // The kernel sets NLM_F_DUMP_INTR on a dump whose socket table changed mid-walk: its
            // contents are inconsistent, so the whole dump is a failed reading.
            if ((nlh->nlmsg_flags & NLM_F_DUMP_INTR) != 0)
            {
                spdlog::debug("inet_diag dump for family {} was interrupted", family);
                return InetDumpOutcome::Failed;
            }

            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                // NLMSG_DONE carries the dump's status: negative is an error part-way through.
                if (NLMSG_PAYLOAD(nlh, 0) >= sizeof(int))
                {
                    int status = 0;
                    std::memcpy(&status, NLMSG_DATA(nlh), sizeof(status));
                    if (status < 0)
                    {
                        spdlog::debug("inet_diag dump for family {} failed: {}", family, safeStrerror(-status));
                        return InetDumpOutcome::Failed;
                    }
                }
                return InetDumpOutcome::Complete;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const auto* err = static_cast<const nlmsgerr*>(NLMSG_DATA(nlh));
                if (err->error == 0)
                {
                    continue; // An ACK, not the end of the dump: keep reading until NLMSG_DONE
                }
                if (err->error == -ENOENT)
                {
                    // The family's (or protocol's) diag module is absent (IPv6 disabled): the dump is
                    // over and has no sockets.
                    return InetDumpOutcome::Unsupported;
                }
                // Any other error (EBUSY, ENOMEM, ...) is a failed reading, not an empty one: taken as
                // complete it would drop every socket's baseline and credit their lifetime bytes when
                // they reappear.
                spdlog::debug("Netlink error for family {}: {}", family, safeStrerror(-err->error));
                return InetDumpOutcome::Failed;
            }

            if (nlh->nlmsg_type == SOCK_DIAG_BY_FAMILY)
            {
                onSocket(static_cast<const void*>(NLMSG_DATA(nlh)), static_cast<std::size_t>(NLMSG_PAYLOAD(nlh, 0)));
            }
        }
#pragma clang diagnostic pop
        // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    }
}

/// Appends the endpoints, state and inode of one inet_diag_msg to `results` (#799).
void parseInetSocketMessage(const void* msg, std::size_t len, int protocol, int family, std::vector<InetSockets::RawInetSocket>& results)
{
    if (len < sizeof(inet_diag_msg))
    {
        return;
    }
    // Copied out rather than read in place, like tcp_info above.
    inet_diag_msg diagMsg{};
    std::memcpy(&diagMsg, msg, sizeof(diagMsg));

    InetSockets::RawInetSocket socket;
    socket.inode = diagMsg.idiag_inode;
    socket.protocol = (protocol == IPPROTO_UDP) ? ConnectionProtocol::Udp : ConnectionProtocol::Tcp;
    socket.family = (family == AF_INET6) ? ConnectionFamily::IPv6 : ConnectionFamily::IPv4;
    socket.state = diagMsg.idiag_state;
    // idiag_src/idiag_dst are __be32[4] (network order; IPv4 in the first word) and the ports __be16:
    // the address bytes are copied as they are, and the ports converted to numbers.
    const std::size_t addressBytes = (socket.family == ConnectionFamily::IPv6) ? socket.local.address.size() : 4;
    std::memcpy(socket.local.address.data(), static_cast<const void*>(diagMsg.id.idiag_src), addressBytes);
    std::memcpy(socket.remote.address.data(), static_cast<const void*>(diagMsg.id.idiag_dst), addressBytes);
    socket.local.port = ntohs(diagMsg.id.idiag_sport);
    socket.remote.port = ntohs(diagMsg.id.idiag_dport);
    results.push_back(socket);
}

} // namespace

std::unique_ptr<INetlinkTransport> makeSockDiagTransport()
{
    auto transport = std::make_unique<SockDiagTransport>();
    if (!transport->isOpen())
    {
        return nullptr;
    }
    return transport;
}

InetDumpOutcome dumpInetSockets(
    INetlinkTransport& transport, std::uint32_t sequence, int protocol, int family, std::vector<InetSockets::RawInetSocket>& results)
{
    alignas(alignof(nlmsghdr)) std::array<std::byte, NETLINK_BUFFER_SIZE> buffer{};
    // No extensions: the endpoints, state and inode are all in the inet_diag_msg header.
    return runInetDiagDump(transport,
                           buffer,
                           sequence,
                           protocol,
                           family,
                           0,
                           [&](const void* message, std::size_t length)
                           { parseInetSocketMessage(message, length, protocol, family, results); });
}

NetlinkSocketStats::NetlinkSocketStats() : NetlinkSocketStats(DEFAULT_SOCKET_STATS_CACHE_TTL)
{}

NetlinkSocketStats::NetlinkSocketStats(std::chrono::milliseconds cacheTtl) : NetlinkSocketStats(makeSockDiagTransport(), cacheTtl)
{}

NetlinkSocketStats::NetlinkSocketStats(std::unique_ptr<INetlinkTransport> transport, std::chrono::milliseconds cacheTtl)
    : m_Transport(std::move(transport)), m_CacheTtl(cacheTtl)
{
    if (!m_Transport)
    {
        return;
    }

    // Issue a best-effort INET_DIAG query as a warm-up / sanity check.
    // Note: availability is currently based solely on successful socket creation/bind;
    // a failure in this initial query does NOT change m_Available. Caught rather than
    // propagated: an exception here (e.g. std::bad_alloc while collecting results) must not
    // escape the constructor (#773).
    try
    {
        std::vector<SocketStats> testResults;
        [[maybe_unused]] const bool complete = queryTcpSockets(testResults);
    }
    catch (const std::exception& e)
    {
        // The logging call itself can allocate (message formatting) and thus throw under the
        // same OOM condition this guard exists for; catching it here too, instead of just the
        // query above, keeps this whole catch path non-throwing.
        try
        {
            spdlog::debug("Netlink warm-up query threw: {}", e.what());
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- intentional: logging is best-effort here,
                    // must not throw
        {}
    }
    catch (...)
    {
        // Non-std::exception throw (unlikely, but the surrounding try/catch's entire purpose
        // is to guarantee this constructor can't throw).
        try
        {
            spdlog::debug("Netlink warm-up query threw a non-standard exception");
        }
        catch (...) // NOLINT(bugprone-empty-catch) -- intentional: logging is best-effort here,
                    // must not throw
        {}
    }

    // Set available - the socket is considered functional if it was created and bound,
    // even if there are no TCP sockets yet or the warm-up query fails.
    m_Available = true;
    spdlog::info("Netlink INET_DIAG available for per-process network monitoring (cache TTL: {}ms)", m_CacheTtl.count());
}

NetlinkSocketStats::~NetlinkSocketStats() noexcept = default;

std::vector<SocketStats> NetlinkSocketStats::queryAllSockets(std::chrono::steady_clock::time_point* sampledAt)
{
    if (!m_Available || !m_Transport)
    {
        return {};
    }

    // Lock for thread-safe cache and socket operations
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) - intentional: socket must be protected
    const std::scoped_lock lock(m_SocketMutex);

    // Check if cache is still valid.
    // Note: Timestamp is intentionally captured AFTER acquiring the lock to ensure
    // consistent cache behavior under concurrent access.
    const auto now = std::chrono::steady_clock::now();
    const auto cacheAge = now - m_LastQueryTime;

    if ((m_CacheTtl.count() > 0) && (m_LastQueryTime != std::chrono::steady_clock::time_point{}) && (cacheAge < m_CacheTtl))
    {
        // Cache hit - return cached results (may be empty if system has no sockets)
        if (sampledAt != nullptr)
        {
            *sampledAt = m_LastSampleTime;
        }
        return m_CachedResults;
    }

    // Cache miss or expired - query the kernel (TCP over IPv4 and IPv6; see the class comment for
    // why UDP isn't queried).
    std::vector<SocketStats> results;
    results.reserve(std::max<std::size_t>(m_CachedResults.size(), 256));
    if (!queryTcpSockets(results))
    {
        // A partial dump isn't a reading: returning it would make every socket it missed look
        // closed, and caching it would serve that for a whole TTL (#1160). The previous complete
        // reading stays cached but expired, so the next call queries again.
        return {};
    }
    m_CachedResults = std::move(results);

    // Update timestamp immediately after kernel query to minimize race window.
    // Only update cache state if caching is enabled (TTL > 0).
    // Skip cache updates when TTL is zero to avoid storing data that won't be used.
    if (m_CacheTtl.count() > 0)
    {
        m_LastQueryTime = now;
    }
    m_LastSampleTime = now;
    if (sampledAt != nullptr)
    {
        *sampledAt = now;
    }

    // Note: Returns a copy of the cached vector. For typical socket counts (<1000),
    // the copy cost is negligible (~microseconds). If this becomes a bottleneck,
    // consider returning a shared_ptr or restructuring to return by const reference.
    return m_CachedResults;
}

std::vector<SocketStats> NetlinkSocketStats::queryAllSocketsUncached()
{
    if (!m_Available || !m_Transport)
    {
        return {};
    }

    // Lock for thread-safe socket operations
    // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection) - intentional: socket must be protected
    const std::scoped_lock lock(m_SocketMutex);

    std::vector<SocketStats> results;
    results.reserve(256);
    if (!queryTcpSockets(results))
    {
        return {};
    }

    // Intentionally NOT updating cache - this is a true bypass for benchmarks/testing
    return results;
}

void NetlinkSocketStats::invalidateCache() noexcept
{
    const std::scoped_lock lock(m_SocketMutex);
    m_CachedResults.clear();
    m_LastQueryTime = {};
}

bool NetlinkSocketStats::queryTcpSockets(std::vector<SocketStats>& results)
{
    // Both families are always dumped; the reading is complete only if both dumps were.
    const bool ipv4Complete = queryDump(IPPROTO_TCP, AF_INET, results);
    const bool ipv6Complete = queryDump(IPPROTO_TCP, AF_INET6, results);
    return ipv4Complete && ipv6Complete;
}

bool NetlinkSocketStats::queryDump(int protocol, int family, std::vector<SocketStats>& results)
{
    // Receive buffer - aligned for netlink messages (nlmsghdr has __u32 fields).
    alignas(alignof(nlmsghdr)) std::array<std::byte, NETLINK_BUFFER_SIZE> buffer{};

    // A fresh sequence number per request, so a straggling reply from an earlier dump can never be
    // mistaken for this one's -- including its NLMSG_DONE, which used to end this dump early.
    const std::uint32_t sequence = m_NextSequence++;
    if (m_NextSequence == 0)
    {
        m_NextSequence = 1;
    }

    // Request the INET_DIAG_INFO extension for tcp_info's byte counters (a bitmask: 1 << (INET_DIAG_INFO - 1)).
    const InetDumpOutcome outcome =
        runInetDiagDump(*m_Transport,
                        buffer,
                        sequence,
                        protocol,
                        family,
                        static_cast<std::uint8_t>(1U << (INET_DIAG_INFO - 1)),
                        [&results](const void* message, std::size_t length) { parseSocketMessageImpl(message, length, results); });
    // A family whose diag module is absent (IPv6 disabled) has no sockets: that's complete.
    return outcome != InetDumpOutcome::Failed;
}

void NetlinkSocketStats::parseSocketMessage(const void* msg, std::size_t len, std::vector<SocketStats>& results)
{
    // Delegate to the implementation in the anonymous namespace
    parseSocketMessageImpl(msg, len, results);
}

std::unordered_map<std::uint64_t, SocketOwner> buildInodeToPidMap(const std::filesystem::path& procRoot)
{
    std::unordered_map<std::uint64_t, SocketOwner> inodeToPid;
    inodeToPid.reserve(1024); // Pre-allocate for typical system

    const std::filesystem::path& procPath = procRoot;
    std::error_code errorCode;

    for (const auto& procEntry : std::filesystem::directory_iterator(procPath, errorCode))
    {
        if (!procEntry.is_directory())
        {
            continue;
        }

        // Check if directory name is a PID (numeric)
        const std::string& pidStr = procEntry.path().filename().string();
        std::int32_t pid = 0;
        auto result = std::from_chars(pidStr.data(), (pidStr.data() + pidStr.size()), pid);
        if (result.ec != std::errc{} || pid <= 0)
        {
            continue;
        }

        // Open /proc/[pid] once and read both its fd links and its stat through that handle: it
        // stays bound to this process, so if the process exits and its PID is reused mid-scan the
        // reads fail rather than mixing the two processes (#1336).
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
        const Posix::FdGuard pidDirFd(::open(procEntry.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (pidDirFd.get() == -1)
        {
            continue; // Process exited
        }

        // Every socket link in /proc/[pid]/fd, through the same handle: the walk LinuxProcessProbe's
        // FD count shares on the samples it rebuilds the map itself (#1426).
        std::optional<std::uint64_t> startTimeTicks; // read once, on the process's first socket
        (void) ProcFdScan::scanFds(pidDirFd.get(),
                                   /*readEveryLink=*/true,
                                   [&](std::uint64_t inode)
                                   {
                                       if (!startTimeTicks.has_value())
                                       {
                                           startTimeTicks = readStartTimeTicksAt(pidDirFd.get());
                                       }
                                       addSocketOwner(inodeToPid, inode, SocketOwner{.pid = pid, .startTimeTicks = *startTimeTicks});
                                   });
    }

    return inodeToPid;
}

void addSocketOwner(std::unordered_map<std::uint64_t, SocketOwner>& inodeToPid, std::uint64_t inode, SocketOwner owner)
{
    // Shared socket: the lowest PID keeps it, whatever order readdir() lists /proc in, so the owner
    // is the same on every rebuild (#1099).
    const auto [it, inserted] = inodeToPid.try_emplace(inode, owner);
    if (!inserted && owner.pid < it->second.pid)
    {
        it->second = owner;
    }
}

std::uint64_t readStartTimeTicksAt(int pidDirFd) noexcept
{
    // Ample: comm is kernel-capped at 15 chars, and the start time is field 22. 0 if the file can't
    // be read (the process exited, or a synthetic /proc without one).
    std::array<char, 1024> buf{};
    const std::size_t len = ProcParsing::readProcFileOnceAt(pidDirFd, "stat", buf.data(), buf.size());
    return ProcParsing::parseStatStartTime(std::string_view(buf.data(), len)).value_or(0);
}

} // namespace Platform

#endif // __linux__ && headers available
