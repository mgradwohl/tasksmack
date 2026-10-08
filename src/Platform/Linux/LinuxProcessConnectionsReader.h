#pragma once

#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/PlatformConfig.h"

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
#include "NetlinkSocketStats.h"
#endif

#include <cstdint>
#include <memory>
#include <string>

namespace Platform
{

/// Linux IProcessConnectionsReader (#799): the process's socket inodes from its /proc/[pid]/fd links,
/// matched against the system's TCP and UDP sockets (IPv4 and IPv6) to give each one's endpoints and
/// state.
///
/// Everything about the process is read through one handle on its /proc/[pid] directory, after its
/// start time is checked against the target's, so a PID reused in between reads as exited rather than
/// showing a stranger's sockets. Reading the fd links needs ptrace access to the process (its own
/// user, or root / CAP_SYS_PTRACE); otherwise the result is PermissionDenied.
///
/// The socket tables come from an INET_DIAG netlink dump (dumpInetSockets()) when the process shares
/// TaskSmack's network namespace and the build has netlink (TASKSMACK_HAS_NETLINK_SOCKET_STATS); a
/// table netlink cannot dump (no udp_diag, netlink unavailable), every table of a process in another
/// network namespace (a container), and every table in a build without netlink are read from the
/// process's own /proc/[pid]/net/{tcp,tcp6,udp,udp6} instead, which show its namespace. Each table is
/// either read whole or known absent (ENOENT: that family disabled); any other failure fails the read
/// (or reports the process exited), never an Ok with some tables missing.
///
/// A socket in TIME_WAIT belongs to no file once its owner closed it, so it never appears here.
///
/// Not thread-safe: one read at a time (the Connections view runs them one at a time on a worker).
class LinuxProcessConnectionsReader final : public IProcessConnectionsReader
{
  public:
    /// A reader of the real /proc that opens its NETLINK_SOCK_DIAG socket on its first read (none at
    /// all if never read).
    LinuxProcessConnectionsReader();

    /// A synthetic /proc tree to read instead of the real one (a test seam).
    struct ProcRoot
    {
        std::string path; ///< "<path>/<pid>/..." stands for "/proc/<pid>/..."
    };

    /// Test seam: read processes under @p root and never use netlink, so every table comes from
    /// <root>/<pid>/net.
    explicit LinuxProcessConnectionsReader(ProcRoot root);

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    /// Test seam: dump through @p transport; a null one reads every table from /proc/[pid]/net.
    explicit LinuxProcessConnectionsReader(std::unique_ptr<INetlinkTransport> transport);
#endif

    ~LinuxProcessConnectionsReader() override;

    LinuxProcessConnectionsReader(const LinuxProcessConnectionsReader&) = delete;
    LinuxProcessConnectionsReader& operator=(const LinuxProcessConnectionsReader&) = delete;
    LinuxProcessConnectionsReader(LinuxProcessConnectionsReader&&) = delete;
    LinuxProcessConnectionsReader& operator=(LinuxProcessConnectionsReader&&) = delete;

    [[nodiscard]] bool hasConnections() const override;
    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& target) override;

  private:
    std::string m_ProcRoot = "/proc";

#if TASKSMACK_HAS_NETLINK_SOCKET_STATS
    /// The netlink transport, opened on first use unless one was injected; null when unavailable.
    [[nodiscard]] INetlinkTransport* transport();

    std::unique_ptr<INetlinkTransport> m_Transport;
    bool m_TransportOpened = false; // a transport was created (or injected): don't try again
    std::uint32_t m_NextSequence = 1;
#endif
};

} // namespace Platform
