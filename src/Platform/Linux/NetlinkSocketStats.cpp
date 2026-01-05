// Only compile on Linux with required headers
#if defined(__linux__) && __has_include(<linux/inet_diag.h>) && __has_include(<linux/sock_diag.h>)

#include "NetlinkSocketStats.h"

#include <spdlog/spdlog.h>

#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <system_error>

#include <dirent.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Platform
{

namespace
{

// Buffer size for netlink messages (should be large enough for typical response)
constexpr std::size_t NETLINK_BUFFER_SIZE = 65536;

/// Thread-safe strerror wrapper using strerror_r
[[nodiscard]] std::string safeStrerror(int errnum)
{
    std::array<char, 256> buffer{};
    // Use GNU-specific strerror_r which returns char*
    // The returned pointer may point to buffer or to a static string
    const char* result = strerror_r(errnum, buffer.data(), buffer.size());
    return std::string(result);
}

// Request structure for inet_diag with extensions
struct InetDiagRequest
{
    nlmsghdr nlh;
    inet_diag_req_v2 req;
};

/// Parse rtattr chain following inet_diag_msg to extract tcp_info byte counters
void parseTcpInfo(const inet_diag_msg* diagMsg, std::size_t msgLen, SocketStats& stats)
{
    // Calculate where attributes start (after the inet_diag_msg)
    const auto* attrStart = reinterpret_cast<const std::uint8_t*>(diagMsg + 1);
    std::size_t attrLen = msgLen - sizeof(inet_diag_msg);

    // Ensure we have room for attributes
    if (attrLen < sizeof(rtattr))
    {
        return;
    }

    // Walk through the attributes
    // Note: Suppress alignment warning - kernel netlink macros use char* internally
    // which is safe because the kernel guarantees proper alignment in netlink messages
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
    for (const auto* rta = reinterpret_cast<const rtattr*>(attrStart); RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen))
    {
        if (rta->rta_type == INET_DIAG_INFO)
        {
            // This attribute contains tcp_info structure
            const auto* tcpInfo = static_cast<const tcp_info*>(RTA_DATA(rta));
            const std::size_t infoLen = RTA_PAYLOAD(rta);

            // Check we have enough data for the byte counter fields
            // bytes_acked and bytes_received were added in Linux 4.2
            // They're at offset ~144 bytes into tcp_info
            if (infoLen >= offsetof(tcp_info, tcpi_bytes_received) + sizeof(tcpInfo->tcpi_bytes_received))
            {
                stats.bytesReceived = tcpInfo->tcpi_bytes_received;
            }
            if (infoLen >= offsetof(tcp_info, tcpi_bytes_acked) + sizeof(tcpInfo->tcpi_bytes_acked))
            {
                stats.bytesSent = tcpInfo->tcpi_bytes_acked;
            }
            break;
        }
    }
#pragma clang diagnostic pop
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
}

} // namespace

NetlinkSocketStats::NetlinkSocketStats()
{
    // Create netlink socket for SOCK_DIAG
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer) - conditional initialization
    m_Socket = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (m_Socket < 0)
    {
        spdlog::debug("Failed to create NETLINK_SOCK_DIAG socket: {}", safeStrerror(errno));
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
        close(m_Socket);
        m_Socket = -1;
        return;
    }

    // Test if INET_DIAG works by doing a simple query
    std::vector<SocketStats> testResults;
    querySockets(IPPROTO_TCP, testResults);
    m_Available = true;

    spdlog::info("Netlink INET_DIAG available for per-process network monitoring");
}

NetlinkSocketStats::~NetlinkSocketStats()
{
    if (m_Socket >= 0)
    {
        close(m_Socket);
    }
}

std::vector<SocketStats> NetlinkSocketStats::queryAllSockets()
{
    std::vector<SocketStats> results;
    results.reserve(256); // Reasonable initial capacity

    if (!m_Available || m_Socket < 0)
    {
        return results;
    }

    // Query TCP sockets (IPv4 and IPv6)
    querySockets(IPPROTO_TCP, results);

    // Query UDP sockets (IPv4 and IPv6)
    // Note: UDP may have limited byte counter support
    querySockets(IPPROTO_UDP, results);

    return results;
}

// NOLINTNEXTLINE(readability-make-member-function-const) - modifies socket state via send/recv
void NetlinkSocketStats::querySockets(int protocol, std::vector<SocketStats>& results)
{
    // Build the request for IPv4
    InetDiagRequest req{};
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;

    req.req.sdiag_family = AF_INET;
    req.req.sdiag_protocol = static_cast<std::uint8_t>(protocol);
    req.req.idiag_states = static_cast<std::uint32_t>(-1); // All states

    // Request INET_DIAG_INFO extension to get tcp_info with byte counters
    // This is a bitmask: (1 << (INET_DIAG_INFO - 1))
    req.req.idiag_ext = (1U << (INET_DIAG_INFO - 1));

    // Send request
    if (send(m_Socket, &req, sizeof(req), 0) < 0)
    {
        spdlog::debug("Failed to send inet_diag request: {}", safeStrerror(errno));
        return;
    }

    // Receive response - use aligned buffer for netlink messages
    // Netlink messages require 4-byte alignment (nlmsghdr has __u32 fields)
    alignas(alignof(nlmsghdr)) std::array<char, NETLINK_BUFFER_SIZE> buffer{};
    bool done = false;

    while (!done)
    {
        const ssize_t len = recv(m_Socket, buffer.data(), buffer.size(), 0);
        if (len < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            spdlog::debug("Failed to receive inet_diag response: {}", safeStrerror(errno));
            break;
        }

        // Parse netlink messages
        // Suppress alignment warning - buffer is properly aligned above and kernel
        // netlink protocol guarantees proper alignment of messages
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
        auto remainingLen = static_cast<std::size_t>(len);
        for (auto* nlh = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(nlh, remainingLen); nlh = NLMSG_NEXT(nlh, remainingLen))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = true;
                break;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const auto* err = static_cast<nlmsgerr*>(NLMSG_DATA(nlh));
                if (err->error != 0)
                {
                    spdlog::debug("Netlink error: {}", safeStrerror(-err->error));
                }
                done = true;
                break;
            }

            if (nlh->nlmsg_type == SOCK_DIAG_BY_FAMILY)
            {
                parseSocketMessage(NLMSG_DATA(nlh), NLMSG_PAYLOAD(nlh, 0), results);
            }
        }
#pragma clang diagnostic pop
    }

    // Also query IPv6 sockets
    req.req.sdiag_family = AF_INET6;
    req.nlh.nlmsg_seq = 2;

    if (send(m_Socket, &req, sizeof(req), 0) < 0)
    {
        return;
    }

    done = false;
    while (!done)
    {
        const ssize_t len = recv(m_Socket, buffer.data(), buffer.size(), 0);
        if (len < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
        auto remainingLen = static_cast<std::size_t>(len);
        for (auto* nlh = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(nlh, remainingLen); nlh = NLMSG_NEXT(nlh, remainingLen))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = true;
                break;
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                done = true;
                break;
            }

            if (nlh->nlmsg_type == SOCK_DIAG_BY_FAMILY)
            {
                parseSocketMessage(NLMSG_DATA(nlh), NLMSG_PAYLOAD(nlh, 0), results);
            }
        }
#pragma clang diagnostic pop
    }
}

void NetlinkSocketStats::parseSocketMessage(const void* msg, std::size_t len, std::vector<SocketStats>& results)
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

std::unordered_map<std::uint64_t, std::int32_t> buildInodeToPidMap()
{
    std::unordered_map<std::uint64_t, std::int32_t> inodeToPid;
    inodeToPid.reserve(1024); // Pre-allocate for typical system

    const std::filesystem::path procPath("/proc");
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
        auto result = std::from_chars(pidStr.data(), pidStr.data() + pidStr.size(), pid);
        if (result.ec != std::errc{} || pid <= 0)
        {
            continue;
        }

        // Scan /proc/[pid]/fd/ for socket symlinks
        const std::filesystem::path fdPath = procEntry.path() / "fd";

        // Use opendir/readdir for efficiency (avoid exception overhead)
        DIR* fdDir = opendir(fdPath.c_str());
        if (fdDir == nullptr)
        {
            continue; // Permission denied or process exited
        }

        std::array<char, 256> linkTarget{};

        // NOLINTNEXTLINE(concurrency-mt-unsafe) - readdir is safe here: single DIR* per thread
        while (struct dirent* entry = readdir(fdDir))
        {
            // Skip . and ..
            if (entry->d_name[0] == '.')
            {
                continue;
            }

            // Read the symlink target
            const std::string fdFilePath = fdPath.string() + "/" + entry->d_name;
            const ssize_t linkLen = readlink(fdFilePath.c_str(), linkTarget.data(), linkTarget.size() - 1);
            if (linkLen <= 0)
            {
                continue;
            }
            linkTarget[static_cast<std::size_t>(linkLen)] = '\0';

            // Check if it's a socket: "socket:[inode]"
            const std::string_view target(linkTarget.data(), static_cast<std::size_t>(linkLen));
            if (!target.starts_with("socket:["))
            {
                continue;
            }

            // Extract inode number
            const std::size_t start = 8; // Length of "socket:["
            const std::size_t end = target.find(']', start);
            if (end == std::string_view::npos)
            {
                continue;
            }

            std::uint64_t inode = 0;
            auto parseResult = std::from_chars(target.data() + start, target.data() + end, inode);
            if (parseResult.ec == std::errc{} && inode != 0)
            {
                inodeToPid[inode] = pid;
            }
        }

        closedir(fdDir);
    }

    return inodeToPid;
}

std::unordered_map<std::int32_t, std::pair<std::uint64_t, std::uint64_t>>
aggregateByPid(const std::vector<SocketStats>& sockets, const std::unordered_map<std::uint64_t, std::int32_t>& inodeToPid)
{
    std::unordered_map<std::int32_t, std::pair<std::uint64_t, std::uint64_t>> pidStats;

    for (const auto& socket : sockets)
    {
        auto it = inodeToPid.find(socket.inode);
        if (it == inodeToPid.end())
        {
            continue; // Socket not mapped to any process (might be kernel)
        }

        const std::int32_t pid = it->second;
        auto& [received, sent] = pidStats[pid];
        received += socket.bytesReceived;
        sent += socket.bytesSent;
    }

    return pidStats;
}

} // namespace Platform

#endif // __linux__ && headers available
