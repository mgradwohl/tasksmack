#include "LinuxProcessConnectionsReader.h"

#include "InetSocketTable.h"
#include "NetlinkSocketStats.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "PosixGuards.h"
#include "ProcFdScan.h"
#include "ProcParsing.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX/Linux headers: include-cleaner lacks mappings for IPPROTO_*, AF_*, ssize_t
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// One of the four socket tables: what to dump over netlink, and the /proc/[pid]/net file that holds it.
struct SocketTable
{
    ConnectionProtocol protocol;
    ConnectionFamily family;
    int ipProtocol;
    int addressFamily;
    const char* procFile;
};

constexpr std::array<SocketTable, 4> SOCKET_TABLES{{
    {.protocol = ConnectionProtocol::Tcp,
     .family = ConnectionFamily::IPv4,
     .ipProtocol = IPPROTO_TCP,
     .addressFamily = AF_INET,
     .procFile = "net/tcp"},
    {.protocol = ConnectionProtocol::Tcp,
     .family = ConnectionFamily::IPv6,
     .ipProtocol = IPPROTO_TCP,
     .addressFamily = AF_INET6,
     .procFile = "net/tcp6"},
    {.protocol = ConnectionProtocol::Udp,
     .family = ConnectionFamily::IPv4,
     .ipProtocol = IPPROTO_UDP,
     .addressFamily = AF_INET,
     .procFile = "net/udp"},
    {.protocol = ConnectionProtocol::Udp,
     .family = ConnectionFamily::IPv6,
     .ipProtocol = IPPROTO_UDP,
     .addressFamily = AF_INET6,
     .procFile = "net/udp6"},
}};

[[nodiscard]] constexpr ConnectionsReadStatus statusFromErrno(int err) noexcept
{
    switch (err)
    {
    case EACCES:
    case EPERM:
        return ConnectionsReadStatus::PermissionDenied;
    case ENOENT:
    case ESRCH:
        return ConnectionsReadStatus::ProcessExited;
    default:
        return ConnectionsReadStatus::Failed;
    }
}

[[nodiscard]] ConnectionsReadResult failure(ConnectionsReadStatus status)
{
    return {.status = status, .connections = {}};
}

/// The target of the symlink @p name in the directory @p dirFd ("net:[4026531840]" for ns/net), or
/// empty if it can't be read.
[[nodiscard]] std::string readLinkAt(int dirFd, const char* name)
{
    std::array<char, 64> target{};
    const auto length = ::readlinkat(dirFd, name, target.data(), target.size());
    if (length <= 0 || static_cast<std::size_t>(length) >= target.size())
    {
        return {};
    }
    return {target.data(), static_cast<std::size_t>(length)};
}

/// Whether the process whose /proc/[pid] directory @p pidDirFd is open on shares TaskSmack's network
/// namespace, so a netlink dump (which sees only TaskSmack's) lists its sockets. False when either
/// link can't be read: the process's own /proc/[pid]/net tables are right in every namespace.
[[nodiscard]] bool sharesOurNetworkNamespace(int pidDirFd)
{
    const std::string theirs = readLinkAt(pidDirFd, "ns/net");
    const std::string ours = readLinkAt(AT_FDCWD, "/proc/self/ns/net");
    return !theirs.empty() && theirs == ours;
}

/// Every socket of @p table from the process's own /proc/[pid]/net file; false if it can't be read
/// (absent: IPv6 disabled).
[[nodiscard]] bool readProcNetTable(int pidDirFd, const SocketTable& table, std::vector<InetSockets::RawInetSocket>& sockets)
{
    const std::vector<char> contents = ProcParsing::readProcFileFullAt(pidDirFd, table.procFile);
    if (contents.empty())
    {
        return false; // even a table with no sockets has its header line
    }
    InetSockets::parseProcNetTable(std::string_view(contents.data(), contents.size()), table.protocol, table.family, sockets);
    return true;
}

} // namespace

LinuxProcessConnectionsReader::LinuxProcessConnectionsReader() = default;

LinuxProcessConnectionsReader::LinuxProcessConnectionsReader(std::unique_ptr<INetlinkTransport> transport)
    : m_Transport(std::move(transport)), m_TransportOpened(true)
{}

LinuxProcessConnectionsReader::~LinuxProcessConnectionsReader() = default;

bool LinuxProcessConnectionsReader::hasConnections() const
{
    return true;
}

INetlinkTransport* LinuxProcessConnectionsReader::transport()
{
    if (!m_TransportOpened)
    {
        m_TransportOpened = true;
        m_Transport = makeSockDiagTransport(); // null if netlink is unavailable: /proc/[pid]/net it is
    }
    return m_Transport.get();
}

ConnectionsReadResult LinuxProcessConnectionsReader::readConnections(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ConnectionsReadStatus::ProcessExited);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity: refused rather than read by PID alone, as checkProcessIdentity() refuses
        // an action. The caller retries at its next refresh, once the process's start time is known.
        return failure(ConnectionsReadStatus::IdentityUnknown);
    }

    // One handle on the process's /proc directory: once open it keeps naming this process, so a file
    // read through it after the process exits fails instead of reaching whatever process is given the
    // PID next.
    const std::string dirPath = "/proc/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(statusFromErrno(errno));
    }
    const std::uint64_t startTicks = readStartTimeTicksAt(dir.get());
    if (startTicks == 0 || startTicks != target.startTimeTicks)
    {
        return failure(ConnectionsReadStatus::ProcessExited); // gone, or the PID now names another process
    }

    // Opened first on its own for its errno: scanFds() only says the listing failed, and another user's
    // process (EACCES) must read as "not permitted", not as a failure.
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
        const FdGuard fdDir(::openat(dir.get(), "fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        if (fdDir.get() < 0)
        {
            return failure(statusFromErrno(errno));
        }
    }
    std::vector<std::uint64_t> inodes;
    const ProcFdScan::FdScan scan =
        ProcFdScan::scanFds(dir.get(), /*readEveryLink=*/true, [&inodes](std::uint64_t inode) { inodes.push_back(inode); });
    if (scan.linkAccess == ProcFdScan::LinkAccess::Denied)
    {
        return failure(ConnectionsReadStatus::PermissionDenied);
    }
    if (!scan.listed)
    {
        return failure(readStartTimeTicksAt(dir.get()) == 0 ? ConnectionsReadStatus::ProcessExited : ConnectionsReadStatus::Failed);
    }
    std::ranges::sort(inodes);
    const auto duplicates = std::ranges::unique(inodes); // one socket open on several fds is one row
    inodes.erase(duplicates.begin(), duplicates.end());
    if (inodes.empty())
    {
        return {.status = ConnectionsReadStatus::Ok, .connections = {}}; // no sockets: nothing to dump
    }

    std::vector<InetSockets::RawInetSocket> sockets;
    INetlinkTransport* netlink = sharesOurNetworkNamespace(dir.get()) ? transport() : nullptr;
    std::size_t tablesRead = 0;
    for (const SocketTable& table : SOCKET_TABLES)
    {
        if (netlink != nullptr)
        {
            const std::size_t before = sockets.size();
            const std::uint32_t sequence = m_NextSequence++;
            if (m_NextSequence == 0)
            {
                m_NextSequence = 1;
            }
            const InetDumpOutcome outcome = dumpInetSockets(*netlink, sequence, table.ipProtocol, table.addressFamily, sockets);
            if (outcome == InetDumpOutcome::Complete)
            {
                ++tablesRead;
                continue;
            }
            sockets.resize(before); // a partial or refused dump: this table comes from /proc instead
            if (outcome == InetDumpOutcome::Failed)
            {
                // A timed-out or broken dump: the rest of this read uses /proc too, so a stalled
                // kernel costs this (UI-thread) read one receive timeout, not one per table.
                netlink = nullptr;
            }
        }
        if (readProcNetTable(dir.get(), table, sockets))
        {
            ++tablesRead;
        }
    }
    if (tablesRead == 0)
    {
        return failure(ConnectionsReadStatus::Failed);
    }

    ConnectionsReadResult result{.status = ConnectionsReadStatus::Ok, .connections = {}};
    for (const InetSockets::RawInetSocket& socket : sockets)
    {
        // Inode 0 is a socket no file holds (TIME_WAIT): never this process's.
        if (socket.inode != 0 && std::ranges::binary_search(inodes, socket.inode))
        {
            result.connections.push_back(InetSockets::toConnection(socket));
        }
    }
    return result;
}

} // namespace Platform
