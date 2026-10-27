#pragma once

// Every TCP and UDP socket on the machine, with the process holding it (#3066): the Connections tab.
// Linux dumps the kernel's socket tables (netlink INET_DIAG, else /proc/net) and finds each socket's
// owner from the processes' file descriptors; Windows (the Windows lane's part) would read
// GetExtendedTcpTable and GetExtendedUdpTable.
//
// Sampled on the Connections tab's own worker, at the Settings interval, only while the tab is shown.
// The App composition root creates the reader (Platform::makeSystemConnectionsReader()).

#include "Platform/IProcessConnections.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// One socket and the process holding it.
struct SystemConnection
{
    ProcessConnection connection;
    std::int32_t pid = 0;             ///< 0 when no process TaskSmack can see holds it (TIME_WAIT, another user's)
    std::uint64_t startTimeTicks = 0; ///< The holder's start time, with pid its identity; 0 when unknown
    std::string processName;          ///< The holder's name (comm), or empty
};

/// What came of one read.
enum class SystemConnectionsStatus : std::uint8_t
{
    Ok,          ///< `connections` holds every socket that could be read.
    Unsupported, ///< This run can't list the machine's sockets (synthetic runs, Windows for now).
    Failed,      ///< No socket table could be read.
};

struct SystemConnectionsRead
{
    SystemConnectionsStatus status = SystemConnectionsStatus::Unsupported;
    std::vector<SystemConnection> connections;
    std::string detail; ///< For Failed, what failed; else empty.
};

/// Lists every TCP and UDP socket on request.
class ISystemConnectionsReader
{
  public:
    virtual ~ISystemConnectionsReader() = default;

    ISystemConnectionsReader() = default;
    ISystemConnectionsReader(const ISystemConnectionsReader&) = default;
    ISystemConnectionsReader& operator=(const ISystemConnectionsReader&) = default;
    ISystemConnectionsReader(ISystemConnectionsReader&&) = default;
    ISystemConnectionsReader& operator=(ISystemConnectionsReader&&) = default;

    /// Whether this run can list the machine's sockets. When false the Connections tab says so.
    [[nodiscard]] virtual bool hasConnections() const = 0;

    /// Reads every socket now. Synchronous and possibly slow (every process's descriptors are walked):
    /// called from the tab's sampler thread, never the UI thread.
    [[nodiscard]] virtual SystemConnectionsRead read() = 0;
};

/// The reader for a run that can't list the machine's sockets.
class UnsupportedSystemConnectionsReader final : public ISystemConnectionsReader
{
  public:
    [[nodiscard]] bool hasConnections() const override
    {
        return false;
    }
    [[nodiscard]] SystemConnectionsRead read() override
    {
        return {};
    }
};

} // namespace Platform
