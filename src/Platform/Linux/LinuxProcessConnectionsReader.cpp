#include "LinuxProcessConnectionsReader.h"

#include "InetSocketTable.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/PlatformConfig.h"
#include "PosixGuards.h"
#include "ProcFdScan.h"
#include "ProcParsing.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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

[[nodiscard]] ConnectionsReadResult failure(ConnectionsReadStatus status, std::string detail = {})
{
    return {.status = status, .connections = {}, .detail = std::move(detail)};
}

/// The start time (/proc/[pid]/stat field 22) from the stat file in the /proc/[pid] directory
/// @p pidDirFd is open on; 0 if it can't be read (the process exited).
[[nodiscard]] std::uint64_t startTimeTicksAt(int pidDirFd) noexcept
{
    std::array<char, 1024> buf{};
    const std::size_t len = ProcParsing::readProcFileOnceAt(pidDirFd, "stat", buf.data(), buf.size());
    return ProcParsing::parseStatStartTime(std::string_view(buf.data(), len)).value_or(0);
}

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
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
#endif

/// The whole of the file @p name in the directory @p dirFd, or the errno of the open or read that
/// failed. Read to EOF in chunks: a /proc/net table is served a page at a time.
[[nodiscard]] std::expected<std::string, int> readWholeFileAt(int dirFd, const char* name)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX openat() is variadic
    const FdGuard fd(::openat(dirFd, name, O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::unexpected(errno);
    }
    std::string contents;
    std::array<char, 8192> chunk{};
    for (;;)
    {
        const auto n = ::read(fd.get(), chunk.data(), chunk.size());
        if (n == 0)
        {
            return contents;
        }
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // interrupted by a signal: retry
            }
            return std::unexpected(errno);
        }
        contents.append(chunk.data(), static_cast<std::size_t>(n));
    }
}

/// Every socket of @p table from the process's own /proc/[pid]/net file, appended to @p sockets.
/// Returns 0 when the table was read whole or is absent (ENOENT: its family is disabled, e.g. no
/// IPv6 -- complete, with no sockets); otherwise the errno of the open or read that failed.
[[nodiscard]] int readProcNetTable(int pidDirFd, const SocketTable& table, std::vector<InetSockets::RawInetSocket>& sockets)
{
    const std::expected<std::string, int> contents = readWholeFileAt(pidDirFd, table.procFile);
    if (!contents.has_value())
    {
        return contents.error() == ENOENT ? 0 : contents.error();
    }
    InetSockets::parseProcNetTable(*contents, table.protocol, table.family, sockets);
    return 0;
}

} // namespace

LinuxProcessConnectionsReader::LinuxProcessConnectionsReader() = default;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
// Never netlink (opened, and null): a synthetic tree's sockets are not the kernel's.
LinuxProcessConnectionsReader::LinuxProcessConnectionsReader(ProcRoot root)
    : m_ProcRoot(std::move(root.path)), m_AfterFdScan(std::move(root.afterFdScan)), m_TransportOpened(true)
{}
#else
LinuxProcessConnectionsReader::LinuxProcessConnectionsReader(ProcRoot root)
    : m_ProcRoot(std::move(root.path)), m_AfterFdScan(std::move(root.afterFdScan))
{}
#endif

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
LinuxProcessConnectionsReader::LinuxProcessConnectionsReader(std::unique_ptr<INetlinkTransport> transport)
    : m_Transport(std::move(transport)), m_TransportOpened(true)
{}

INetlinkTransport* LinuxProcessConnectionsReader::transport()
{
    if (!m_TransportOpened)
    {
        m_TransportOpened = true;
        m_Transport = makeSockDiagTransport(); // null if netlink is unavailable: /proc/[pid]/net it is
    }
    return m_Transport.get();
}
#endif

LinuxProcessConnectionsReader::~LinuxProcessConnectionsReader() = default;

bool LinuxProcessConnectionsReader::hasConnections() const
{
    return true;
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
    const std::string dirPath = m_ProcRoot + "/" + std::to_string(target.pid);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    const FdGuard dir(::open(dirPath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (dir.get() < 0)
    {
        return failure(statusFromErrno(errno));
    }
    // The target's identity, through the same handle: checked before anything is read, and again
    // whenever a read fails or completes, so neither a failure nor a short read of an exiting process
    // passes for the process's sockets.
    const auto stillTarget = [&dir, &target]()
    {
        return startTimeTicksAt(dir.get()) == target.startTimeTicks;
    };
    if (!stillTarget())
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
    if (m_AfterFdScan)
    {
        m_AfterFdScan(); // test seam (ProcRoot): the process "exits" here
    }
    if (scan.linkAccess == ProcFdScan::LinkAccess::Denied)
    {
        return failure(ConnectionsReadStatus::PermissionDenied);
    }
    if (!scan.listed)
    {
        return failure(stillTarget() ? ConnectionsReadStatus::Failed : ConnectionsReadStatus::ProcessExited);
    }
    std::ranges::sort(inodes);
    const auto duplicates = std::ranges::unique(inodes); // one socket open on several fds is one row
    inodes.erase(duplicates.begin(), duplicates.end());
    if (inodes.empty())
    {
        // An empty fd listing may be a process that exited during the scan: confirmed before it is
        // reported as "no sockets", as every other completion is.
        if (!stillTarget())
        {
            return failure(ConnectionsReadStatus::ProcessExited);
        }
        return {.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}}; // no sockets: nothing to dump
    }

    std::vector<InetSockets::RawInetSocket> sockets;
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    INetlinkTransport* netlink = sharesOurNetworkNamespace(dir.get()) ? transport() : nullptr;
#endif
    for (const SocketTable& table : SOCKET_TABLES)
    {
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
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
                continue;
            }
            sockets.resize(before); // a partial or refused dump: this table comes from /proc instead
            if (outcome == InetDumpOutcome::Failed)
            {
                // A timed-out or broken dump: the rest of this read uses /proc too, so a stalled
                // kernel costs this read one receive timeout, not one per table.
                netlink = nullptr;
            }
        }
#endif
        if (const int error = readProcNetTable(dir.get(), table, sockets); error != 0)
        {
            // Never an Ok with this table's sockets missing: the process exited mid-read (its /proc
            // files fail through the handle), or the read itself failed.
            if (!stillTarget())
            {
                return failure(ConnectionsReadStatus::ProcessExited);
            }
            return failure(ConnectionsReadStatus::Failed, std::generic_category().message(error));
        }
    }
    // A table that read as absent (ENOENT) because the process exited, not because its family is
    // disabled, would leave the rows incomplete.
    if (!stillTarget())
    {
        return failure(ConnectionsReadStatus::ProcessExited);
    }

    ConnectionsReadResult result{.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
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
