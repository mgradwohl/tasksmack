#pragma once

#include "Platform/ISystemConnections.h"
#include "Platform/PlatformConfig.h"

#include <cstdint>
#include <memory>
#include <string>

namespace Platform
{

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
class INetlinkTransport;
#endif

/// Linux ISystemConnectionsReader (#3066): every TCP and UDP socket in TaskSmack's network namespace,
/// from netlink INET_DIAG dumps (falling back to /proc/net/{tcp,tcp6,udp,udp6} table by table, as the
/// per-process reader does), each matched to the process holding it by walking every process's
/// /proc/[pid]/fd (buildInodeToPidMap(): a socket open in several processes goes to the lowest PID).
///
/// A socket held by another user's process is listed without a holder unless TaskSmack can read that
/// process's descriptors (root, or CAP_SYS_PTRACE). A socket no file refers to any more (TIME_WAIT, an
/// orphan) has no holder either.
class LinuxSystemConnectionsReader final : public ISystemConnectionsReader
{
  public:
    /// The real /proc; the netlink socket is opened on the first read.
    LinuxSystemConnectionsReader();

    /// Test seam: a synthetic /proc tree at @p procRoot ("<procRoot>/net/tcp" for /proc/net/tcp,
    /// "<procRoot>/<pid>/fd" for a process's descriptors), and never netlink.
    explicit LinuxSystemConnectionsReader(std::string procRoot);

    ~LinuxSystemConnectionsReader() override;

    LinuxSystemConnectionsReader(const LinuxSystemConnectionsReader&) = delete;
    LinuxSystemConnectionsReader& operator=(const LinuxSystemConnectionsReader&) = delete;
    LinuxSystemConnectionsReader(LinuxSystemConnectionsReader&&) = delete;
    LinuxSystemConnectionsReader& operator=(LinuxSystemConnectionsReader&&) = delete;

    [[nodiscard]] bool hasConnections() const override;
    [[nodiscard]] SystemConnectionsRead read() override;

  private:
    std::string m_ProcRoot = "/proc";
#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    [[nodiscard]] INetlinkTransport* transport();
    std::unique_ptr<INetlinkTransport> m_Transport;
    bool m_TransportOpened = false; // true from the first read, or from the test seam (never netlink)
    std::uint32_t m_NextSequence = 1;
#endif
};

} // namespace Platform
