#include "WindowsTcpClose.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "WindowsSocketTables.h"
#include "WindowsTcpRows.h"

#include <spdlog/spdlog.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - iphlpapi types and calls come from its sub-headers via iphlpapi.h

TcpCloseFunctions realTcpCloseFunctions() noexcept
{
    return {.tables = {}, .setTcpEntry = &::SetTcpEntry};
}

ProcessActionResult closeTcpConnectionOf(const TcpCloseFunctions& fns, std::int32_t pid, const TcpConnectionToClose& connection)
{
    if (fns.setTcpEntry == nullptr)
    {
        return ProcessActionResult::error("Closing a connection isn't available on this system");
    }
    // A listener, or a socket bound but never connected: its remote end is all zero, and there is no connection.
    if (connection.remotePort == 0 || connection.localPort == 0)
    {
        return ProcessActionResult::error("This socket has no remote end, so there is no connection to close");
    }

    // The table again, just before acting: the connection may have closed, or passed to another process, since the
    // Connections tab listed it.
    std::vector<std::byte> buffer;
    const SocketTableCopy copy = copySocketTable(fns.tables, SocketTable::Tcp4, buffer);
    if (!copy.ok)
    {
        return ProcessActionResult::error(std::format("Couldn't read the TCP connection table: {}", copy.detail));
    }
    std::optional<MIB_TCPROW_OWNER_PID> held;
    auto onRow = [&held, &connection, pid](const MIB_TCPROW_OWNER_PID& row)
    {
        if (!held.has_value() && std::cmp_equal(row.dwOwningPid, pid) && tcpConnectionToClose(toConnection(row)) == connection)
        {
            held = row;
        }
    };
    forEachSocketRow<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(buffer, onRow);
    if (!held.has_value())
    {
        return ProcessActionResult::error(std::format("PID {} no longer holds this connection", pid));
    }
    const ConnectionState state = toConnectionState(held->dwState);
    if (state == ConnectionState::Listen)
    {
        return ProcessActionResult::error("This socket is listening for connections, so there is no connection to close");
    }
    if (state == ConnectionState::TimeWait || state == ConnectionState::Closed)
    {
        return ProcessActionResult::error("The connection is already closed");
    }

    MIB_TCPROW row = toTcpRow(*held);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - Windows API requires union access
    row.dwState = static_cast<DWORD>(MIB_TCP_STATE_DELETE_TCB);
    const DWORD error = fns.setTcpEntry(&row);
    if (error == NO_ERROR)
    {
        spdlog::info(
            "Closed PID {}'s TCP connection from local port {} to remote port {}", pid, connection.localPort, connection.remotePort);
        return ProcessActionResult::ok();
    }
    if (error == ERROR_ACCESS_DENIED || error == SET_TCP_ENTRY_NOT_ADMINISTRATOR)
    {
        return ProcessActionResult::denied(CLOSE_CONNECTION_NEEDS_ADMINISTRATOR);
    }
    if (error == ERROR_NOT_FOUND)
    {
        return ProcessActionResult::error(std::format("PID {} no longer holds this connection", pid));
    }
    std::string message = std::format("Windows refused to close it: {}", std::system_category().message(static_cast<int>(error)));
    spdlog::warn("{}", message);
    return ProcessActionResult::error(std::move(message));
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
