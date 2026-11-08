#include "WindowsSocketTables.h"

#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// An IPv4 address DWORD (network byte order in memory) as the first 4 bytes of an endpoint address.
[[nodiscard]] std::array<std::uint8_t, 16> ipv4Address(DWORD address) noexcept
{
    std::array<std::uint8_t, 16> bytes{};
    const auto v4 = std::bit_cast<std::array<std::uint8_t, 4>>(address);
    std::ranges::copy(v4, bytes.begin());
    return bytes;
}

[[nodiscard]] std::array<std::uint8_t, 16> ipv6Address(const UCHAR (&address)[16]) noexcept // NOLINT(*-avoid-c-arrays) - the MIB row's type
{
    return std::bit_cast<std::array<std::uint8_t, 16>>(address);
}

/// A UDP socket bound to @p local: no remote end (all zero) and Closed, shown as UNCONN.
[[nodiscard]] ProcessConnection unconnectedUdp(ConnectionFamily family, const ConnectionEndpoint& local) noexcept
{
    ProcessConnection connection;
    connection.protocol = ConnectionProtocol::Udp;
    connection.family = family;
    connection.local = local;
    connection.state = ConnectionState::Closed;
    return connection;
}

/// One GetExtended*Table call into @p data: the table's family and class bound in.
[[nodiscard]] DWORD callTable(const SocketTableFunctions& api, SocketTable table, PVOID data, PDWORD size)
{
    switch (table)
    {
    case SocketTable::Tcp4:
        return api.getExtendedTcpTable(data, size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    case SocketTable::Tcp6:
        return api.getExtendedTcpTable(data, size, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0);
    case SocketTable::Udp4:
        return api.getExtendedUdpTable(data, size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    case SocketTable::Udp6:
        return api.getExtendedUdpTable(data, size, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0);
    }
    return ERROR_INVALID_PARAMETER;
}

} // namespace

SocketTableCopy copySocketTable(const SocketTableFunctions& api, SocketTable table, std::vector<std::byte>& buffer)
{
    if (buffer.size() < INITIAL_SOCKET_TABLE_BYTES)
    {
        buffer.resize(INITIAL_SOCKET_TABLE_BYTES);
    }
    for (int attempt = 1; attempt <= MAX_TABLE_ATTEMPTS; ++attempt)
    {
        auto size = static_cast<DWORD>(buffer.size());
        const DWORD status = callTable(api, table, buffer.data(), &size);
        if (status == NO_ERROR)
        {
            return {.ok = true, .error = NO_ERROR, .detail = {}};
        }
        if (status != ERROR_INSUFFICIENT_BUFFER)
        {
            return {.ok = false, .error = status, .detail = std::system_category().message(static_cast<int>(status))};
        }
        // Room for the sockets opened before the next call, too.
        buffer.resize(static_cast<std::size_t>(size) + (static_cast<std::size_t>(size) / 4) + 1024);
    }
    return {.ok = false, .error = ERROR_INSUFFICIENT_BUFFER, .detail = "the socket table kept growing while it was read"};
}

ConnectionState toConnectionState(DWORD mibState) noexcept
{
    switch (mibState)
    {
    case MIB_TCP_STATE_CLOSED:
    case MIB_TCP_STATE_DELETE_TCB:
        return ConnectionState::Closed;
    case MIB_TCP_STATE_LISTEN:
        return ConnectionState::Listen;
    case MIB_TCP_STATE_SYN_SENT:
        return ConnectionState::SynSent;
    case MIB_TCP_STATE_SYN_RCVD:
        return ConnectionState::SynReceived;
    case MIB_TCP_STATE_ESTAB:
        return ConnectionState::Established;
    case MIB_TCP_STATE_FIN_WAIT1:
        return ConnectionState::FinWait1;
    case MIB_TCP_STATE_FIN_WAIT2:
        return ConnectionState::FinWait2;
    case MIB_TCP_STATE_CLOSE_WAIT:
        return ConnectionState::CloseWait;
    case MIB_TCP_STATE_CLOSING:
        return ConnectionState::Closing;
    case MIB_TCP_STATE_LAST_ACK:
        return ConnectionState::LastAck;
    case MIB_TCP_STATE_TIME_WAIT:
        return ConnectionState::TimeWait;
    default:
        return ConnectionState::Unknown;
    }
}

std::uint16_t toHostPort(DWORD tablePort) noexcept
{
    return ntohs(static_cast<u_short>(tablePort & 0xFFFFU));
}

ProcessConnection toConnection(const MIB_TCPROW_OWNER_PID& row) noexcept
{
    return {
        .protocol = ConnectionProtocol::Tcp,
        .family = ConnectionFamily::IPv4,
        .local = {.address = ipv4Address(row.dwLocalAddr), .port = toHostPort(row.dwLocalPort)},
        .remote = {.address = ipv4Address(row.dwRemoteAddr), .port = toHostPort(row.dwRemotePort)},
        .state = toConnectionState(row.dwState),
    };
}

ProcessConnection toConnection(const MIB_TCP6ROW_OWNER_PID& row) noexcept
{
    return {
        .protocol = ConnectionProtocol::Tcp,
        .family = ConnectionFamily::IPv6,
        .local = {.address = ipv6Address(row.ucLocalAddr), .port = toHostPort(row.dwLocalPort), .scopeId = row.dwLocalScopeId},
        .remote = {.address = ipv6Address(row.ucRemoteAddr), .port = toHostPort(row.dwRemotePort), .scopeId = row.dwRemoteScopeId},
        .state = toConnectionState(row.dwState),
    };
}

ProcessConnection toConnection(const MIB_UDPROW_OWNER_PID& row) noexcept
{
    return unconnectedUdp(ConnectionFamily::IPv4, {.address = ipv4Address(row.dwLocalAddr), .port = toHostPort(row.dwLocalPort)});
}

ProcessConnection toConnection(const MIB_UDP6ROW_OWNER_PID& row) noexcept
{
    return unconnectedUdp(ConnectionFamily::IPv6,
                          {.address = ipv6Address(row.ucLocalAddr), .port = toHostPort(row.dwLocalPort), .scopeId = row.dwLocalScopeId});
}

} // namespace Platform::Windows
