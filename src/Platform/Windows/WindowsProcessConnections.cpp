#include "WindowsProcessConnections.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// The size a socket table's buffer starts at: a desktop's whole TCP table fits in a few KiB.
constexpr std::size_t INITIAL_TABLE_BYTES = std::size_t{16} * 1024;

[[nodiscard]] ConnectionsReadResult failure(ConnectionsReadStatus status, std::string detail = {})
{
    return {.status = status, .connections = {}, .detail = std::move(detail)};
}

/// The system's message for a Win32 error code.
[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// Closes the process handle through the injected CloseHandle (a test's fake handle never reaches the real one).
struct InjectedHandleCloser
{
    ProcessConnectionsFunctions::CloseHandleFn close = nullptr;

    void operator()(HANDLE handle) const noexcept
    {
        close(handle);
    }
};

using ProcessHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, InjectedHandleCloser>;

/// Copies one socket table into @p buffer through @p call (a GetExtended*Table call bound to its
/// family and table class), growing the buffer while the table outgrows it, at most
/// MAX_TABLE_ATTEMPTS times. nullopt on success, else what failed.
template<typename Call> [[nodiscard]] std::optional<std::string> copyTable(Call call, std::vector<std::byte>& buffer)
{
    for (int attempt = 1; attempt <= MAX_TABLE_ATTEMPTS; ++attempt)
    {
        auto size = static_cast<DWORD>(buffer.size());
        const DWORD status = call(buffer.data(), &size);
        if (status == NO_ERROR)
        {
            return std::nullopt;
        }
        if (status != ERROR_INSUFFICIENT_BUFFER)
        {
            return errorText(status);
        }
        // Room for the sockets opened before the next call, too.
        buffer.resize(static_cast<std::size_t>(size) + (static_cast<std::size_t>(size) / 4) + 1024);
    }
    return std::string("the socket table kept growing while it was read");
}

/// Appends the rows of @p table (a copied MIB_*TABLE_OWNER_PID: a row count, then the rows) whose
/// owning PID is @p pid. A count larger than the buffer holds is clamped to what it holds. Rows are
/// copied out rather than read in place.
template<typename TableT, typename RowT> void appendRowsOf(std::span<const std::byte> table, DWORD pid, std::vector<ProcessConnection>& out)
{
    DWORD count = 0;
    if (table.size() < sizeof(count))
    {
        return;
    }
    std::memcpy(&count, table.data(), sizeof(count));
    constexpr std::size_t FIRST_ROW = offsetof(TableT, table);
    const std::size_t fits = table.size() < FIRST_ROW ? 0 : (table.size() - FIRST_ROW) / sizeof(RowT);
    const std::size_t rows = std::min<std::size_t>(count, fits);
    for (std::size_t i = 0; i < rows; ++i)
    {
        RowT row{};
        std::memcpy(&row, table.subspan(FIRST_ROW + (i * sizeof(RowT)), sizeof(RowT)).data(), sizeof(RowT));
        if (row.dwOwningPid == pid)
        {
            out.push_back(toConnection(row));
        }
    }
}

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

} // namespace

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
        .local = {.address = ipv6Address(row.ucLocalAddr), .port = toHostPort(row.dwLocalPort)},
        .remote = {.address = ipv6Address(row.ucRemoteAddr), .port = toHostPort(row.dwRemotePort)},
        .state = toConnectionState(row.dwState),
    };
}

ProcessConnection toConnection(const MIB_UDPROW_OWNER_PID& row) noexcept
{
    return unconnectedUdp(ConnectionFamily::IPv4, {.address = ipv4Address(row.dwLocalAddr), .port = toHostPort(row.dwLocalPort)});
}

ProcessConnection toConnection(const MIB_UDP6ROW_OWNER_PID& row) noexcept
{
    return unconnectedUdp(ConnectionFamily::IPv6, {.address = ipv6Address(row.ucLocalAddr), .port = toHostPort(row.dwLocalPort)});
}

bool WindowsProcessConnectionsReader::hasConnections() const
{
    return true;
}

ConnectionsReadResult WindowsProcessConnectionsReader::readConnections(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return failure(ConnectionsReadStatus::ProcessExited);
    }
    if (target.startTimeTicks == 0)
    {
        // Unconfirmed identity (Idle, System): refused rather than read by PID alone.
        return failure(ConnectionsReadStatus::IdentityUnknown);
    }

    const auto pid = static_cast<DWORD>(target.pid);
    const ProcessHandle process(m_Api.openProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid),
                                InjectedHandleCloser{.close = m_Api.closeHandle});
    if (!process)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_ACCESS_DENIED)
        {
            // The tables would list its sockets, but which process holds the PID cannot be confirmed.
            return failure(ConnectionsReadStatus::PermissionDenied);
        }
        // ERROR_INVALID_PARAMETER: no process has this PID any more.
        return error == ERROR_INVALID_PARAMETER ? failure(ConnectionsReadStatus::ProcessExited)
                                                : failure(ConnectionsReadStatus::Failed, errorText(error));
    }

    FILETIME creation{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (m_Api.getProcessTimes(process.get(), &creation, &exitTime, &kernelTime, &userTime) == 0)
    {
        return failure(ConnectionsReadStatus::Failed, errorText(GetLastError()));
    }
    const std::uint64_t actualTicks =
        (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32U) | static_cast<std::uint64_t>(creation.dwLowDateTime);
    if (actualTicks != target.startTimeTicks)
    {
        return failure(ConnectionsReadStatus::ProcessExited); // the PID now names another process
    }
    // An exited process whose object is still held open: its sockets are gone, and its PID is pinned.
    DWORD exitCode = 0;
    if (m_Api.getExitCodeProcess(process.get(), &exitCode) != 0 && exitCode != STILL_ACTIVE)
    {
        return failure(ConnectionsReadStatus::ProcessExited);
    }

    // The handle stays open from here on, so the PID cannot be reused while the tables are read.
    ConnectionsReadResult result{.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
    std::vector<std::byte> buffer(INITIAL_TABLE_BYTES);
    const auto readTcp = [this, &buffer](ULONG family)
    {
        return copyTable([this, family](PVOID data, PDWORD size)
                         { return m_Api.getExtendedTcpTable(data, size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0); },
                         buffer);
    };
    const auto readUdp = [this, &buffer](ULONG family)
    {
        return copyTable([this, family](PVOID data, PDWORD size)
                         { return m_Api.getExtendedUdpTable(data, size, FALSE, family, UDP_TABLE_OWNER_PID, 0); },
                         buffer);
    };

    // Any table that cannot be read fails the whole read rather than showing a partial list.
    if (auto error = readTcp(AF_INET))
    {
        return failure(ConnectionsReadStatus::Failed, std::move(*error));
    }
    appendRowsOf<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(buffer, pid, result.connections);
    if (auto error = readTcp(AF_INET6))
    {
        return failure(ConnectionsReadStatus::Failed, std::move(*error));
    }
    appendRowsOf<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(buffer, pid, result.connections);
    if (auto error = readUdp(AF_INET))
    {
        return failure(ConnectionsReadStatus::Failed, std::move(*error));
    }
    appendRowsOf<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(buffer, pid, result.connections);
    if (auto error = readUdp(AF_INET6))
    {
        return failure(ConnectionsReadStatus::Failed, std::move(*error));
    }
    appendRowsOf<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>(buffer, pid, result.connections);
    return result;
}

} // namespace Platform::Windows
