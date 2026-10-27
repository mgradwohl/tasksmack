#include "LinuxSystemConnectionsReader.h"

#include "InetSocketTable.h"
#include "Platform/ISystemConnections.h"
#include "Platform/PlatformConfig.h"
#include "ProcParsing.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX: include-cleaner lacks mappings for AT_FDCWD
#include <fcntl.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform
{

namespace
{

/// A process's name from <procRoot>/<pid>/comm, without its newline; empty if it can't be read.
[[nodiscard]] std::string processName(const std::string& procRoot, std::int32_t pid)
{
    const std::string path = procRoot + "/" + std::to_string(pid) + "/comm";
    std::array<char, 64> buf{};
    std::size_t len = ProcParsing::readProcFile(path.c_str(), buf.data(), buf.size());
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\0'))
    {
        --len;
    }
    return {buf.data(), len};
}

} // namespace

LinuxSystemConnectionsReader::LinuxSystemConnectionsReader() = default;

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
// Never netlink (opened, and null): a synthetic tree's sockets are not the kernel's.
LinuxSystemConnectionsReader::LinuxSystemConnectionsReader(std::string procRoot) : m_ProcRoot(std::move(procRoot)), m_TransportOpened(true)
{}

INetlinkTransport* LinuxSystemConnectionsReader::transport()
{
    if (!m_TransportOpened)
    {
        m_TransportOpened = true;
        m_Transport = makeSockDiagTransport(); // null if netlink is unavailable: /proc/net it is
    }
    return m_Transport.get();
}
#else
LinuxSystemConnectionsReader::LinuxSystemConnectionsReader(std::string procRoot) : m_ProcRoot(std::move(procRoot))
{}
#endif

LinuxSystemConnectionsReader::~LinuxSystemConnectionsReader() = default;

bool LinuxSystemConnectionsReader::hasConnections() const
{
    return true;
}

SystemConnectionsRead LinuxSystemConnectionsReader::read()
{
    std::vector<InetSockets::RawInetSocket> sockets;
    std::size_t tablesRead = 0;
    int lastError = 0;
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    INetlinkTransport* netlink = transport();
#endif
    for (const InetSockets::SocketTable& table : InetSockets::SOCKET_TABLES)
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
                ++tablesRead;
                continue;
            }
            sockets.resize(before); // a partial or refused dump: this table comes from /proc instead
            if (outcome == InetDumpOutcome::Failed)
            {
                netlink = nullptr; // a stalled kernel costs this read one timeout, not four
            }
        }
#endif
        // /proc/net/<table> is this process's network namespace, as the netlink dump is.
        const std::string path = m_ProcRoot + "/" + table.procFile;
        const std::expected<std::string, int> contents = ProcParsing::readWholeFileAt(AT_FDCWD, path.c_str());
        if (!contents.has_value())
        {
            if (contents.error() == ENOENT)
            {
                ++tablesRead; // its family is disabled (no IPv6): complete, with no sockets
            }
            else
            {
                lastError = contents.error();
            }
            continue;
        }
        InetSockets::parseProcNetTable(*contents, table.protocol, table.family, sockets);
        ++tablesRead;
    }
    if (tablesRead == 0)
    {
        return {.status = SystemConnectionsStatus::Failed, .connections = {}, .detail = std::generic_category().message(lastError)};
    }

    // Who holds each socket: every process's descriptors, walked once per read.
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    const std::unordered_map<std::uint64_t, SocketOwner> owners = buildInodeToPidMap(m_ProcRoot);
#endif
    std::unordered_map<std::int32_t, std::string> names; // each holder's name, read once per read
    SystemConnectionsRead result{.status = SystemConnectionsStatus::Ok, .connections = {}, .detail = {}};
    result.connections.reserve(sockets.size());
    for (const InetSockets::RawInetSocket& socket : sockets)
    {
        SystemConnection row{.connection = InetSockets::toConnection(socket), .pid = 0, .startTimeTicks = 0, .processName = {}};
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
        // Inode 0 is a socket no file holds (TIME_WAIT): nobody's.
        if (const auto owner = socket.inode != 0 ? owners.find(socket.inode) : owners.end(); owner != owners.end())
        {
            row.pid = owner->second.pid;
            row.startTimeTicks = owner->second.startTimeTicks;
            auto [name, added] = names.try_emplace(row.pid);
            if (added)
            {
                name->second = processName(m_ProcRoot, row.pid);
            }
            row.processName = name->second;
        }
#endif
        result.connections.push_back(std::move(row));
    }
    return result;
}

} // namespace Platform
