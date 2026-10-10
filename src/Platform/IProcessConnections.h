#pragma once

// On-demand read of one process's TCP and UDP sockets (#799): the netstat-style Connections section
// of Process Details.
//
// Not part of the per-sample enumeration (ProcessCounters): a process's connections are read only for
// the process Process Details shows, only while its Connections section is open, and at most every
// Domain::Sampling::PROCESS_CONNECTIONS_REFRESH_MS. The App composition root creates the reader
// (Platform::makeProcessConnectionsReader()) and hands it to the panel, which never calls the
// factory itself.
//
// The rows are raw: addresses as network-order bytes, ports as numbers, a platform-neutral state.
// Formatting them for display (addresses, state names, sort order) is Domain's job
// (Domain/ProcessConnections.h).

#include "Platform/IProcessActions.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// What came of one connections read.
enum class ConnectionsReadStatus : std::uint8_t
{
    Ok,               ///< Read; `connections` holds the sockets (may be empty: no TCP or UDP sockets open).
    PermissionDenied, ///< The OS refused (Linux: EACCES/EPERM on /proc/[pid]/fd; Windows: OpenProcess, so no identity check).
    ProcessExited,    ///< The process is gone, or its PID now belongs to a different process.
    Unsupported,      ///< This platform cannot list another process's sockets (synthetic runs).
    IdentityUnknown,  ///< The target's start time is unknown (0), so the process could not be confirmed; nothing was read.
    Failed,           ///< Any other error (no socket table could be read).
};

/// TCP or UDP.
enum class ConnectionProtocol : std::uint8_t
{
    Tcp,
    Udp,
};

/// The socket's address family.
enum class ConnectionFamily : std::uint8_t
{
    IPv4,
    IPv6,
};

/// A socket's state, as the kernel tracks it. UDP sockets use only Established (connected) and Closed
/// (bound but unconnected), as Linux reports them.
enum class ConnectionState : std::uint8_t
{
    Unknown,
    Established,
    SynSent,
    SynReceived,
    FinWait1,
    FinWait2,
    TimeWait,
    Closed,
    CloseWait,
    LastAck,
    Listen,
    Closing,
};

/// One end of a socket. `address` holds the address in network byte order: the first 4 bytes for
/// IPv4 (the rest zero), all 16 for IPv6. All zero is the wildcard address; port 0 is no port.
struct ConnectionEndpoint
{
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port = 0;
};

/// One TCP or UDP socket the process holds.
struct ProcessConnection
{
    ConnectionProtocol protocol = ConnectionProtocol::Tcp;
    ConnectionFamily family = ConnectionFamily::IPv4;
    ConnectionEndpoint local;
    ConnectionEndpoint remote;
    ConnectionState state = ConnectionState::Unknown;
};

/// The outcome of IProcessConnectionsReader::readConnections(): a status, and the sockets when Ok.
struct ConnectionsReadResult
{
    ConnectionsReadStatus status = ConnectionsReadStatus::Unsupported;
    std::vector<ProcessConnection> connections;
    /// For Failed, what failed, if known (the OS's error message, e.g. "Input/output error"); else empty.
    std::string detail;
};

/// Lists one process's TCP and UDP sockets on request.
class IProcessConnectionsReader
{
  public:
    virtual ~IProcessConnectionsReader() = default;

    IProcessConnectionsReader() = default;
    IProcessConnectionsReader(const IProcessConnectionsReader&) = default;
    IProcessConnectionsReader& operator=(const IProcessConnectionsReader&) = default;
    IProcessConnectionsReader(IProcessConnectionsReader&&) = default;
    IProcessConnectionsReader& operator=(IProcessConnectionsReader&&) = default;

    /// Whether this platform can list a process's sockets at all. When false the UI hides the
    /// Connections section and readConnections() only ever returns Unsupported.
    [[nodiscard]] virtual bool hasConnections() const = 0;

    /// Read @p target's sockets now. Synchronous.
    /// The target's start time is checked against the process holding the PID, so a process that reused
    /// it reports ProcessExited instead of showing a stranger's sockets. An unknown start time (0) is
    /// refused with IdentityUnknown and nothing is read (Platform::checkProcessIdentity()).
    [[nodiscard]] virtual ConnectionsReadResult readConnections(const ProcessTarget& target) = 0;
};

/// The reader for a platform (or run) that cannot list another process's sockets (synthetic runs).
class UnsupportedProcessConnectionsReader final : public IProcessConnectionsReader
{
  public:
    [[nodiscard]] bool hasConnections() const override
    {
        return false;
    }

    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& /*target*/) override
    {
        return {.status = ConnectionsReadStatus::Unsupported, .connections = {}, .detail = {}};
    }
};

} // namespace Platform
