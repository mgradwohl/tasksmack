#include "WindowsProcessConnections.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "WindowsHandles.h"
#include "WindowsProcessIdentity.h"
#include "WindowsSocketTables.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

[[nodiscard]] ConnectionsReadResult failure(ConnectionsReadStatus status, std::string detail = {})
{
    return {.status = status, .connections = {}, .detail = std::move(detail)};
}

} // namespace

bool WindowsProcessConnectionsReader::hasConnections() const
{
    return true;
}

ConnectionsReadResult WindowsProcessConnectionsReader::readConnections(const ProcessTarget& target)
{
    // A refused open is PermissionDenied: the tables would list its sockets, but which process holds
    // the PID cannot be confirmed.
    // An exited process whose object is still held open is refused too (its sockets are gone).
    auto opened = openProcessVerified(
        {
            .openProcess = m_Api.openProcess,
            .getProcessTimes = m_Api.getProcessTimes,
            .closeHandle = m_Api.closeHandle,
            .getExitCodeProcess = m_Api.getExitCodeProcess,
        },
        target,
        PROCESS_QUERY_LIMITED_INFORMATION);
    if (!opened)
    {
        auto refused = toReadFailure<ConnectionsReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);
    const auto pid = static_cast<DWORD>(target.pid);

    // The handle stays open from here on, so the PID cannot be reused while the tables are read.
    ConnectionsReadResult result{.status = ConnectionsReadStatus::Ok, .connections = {}, .detail = {}};
    const SocketTableFunctions tables{.getExtendedTcpTable = m_Api.getExtendedTcpTable, .getExtendedUdpTable = m_Api.getExtendedUdpTable};
    std::vector<std::byte> buffer(INITIAL_SOCKET_TABLE_BYTES);
    auto keepTargets = [pid, &result](const auto& row)
    {
        if (row.dwOwningPid == pid)
        {
            result.connections.push_back(toConnection(row));
        }
    };

    // Any table that cannot be read fails the whole read rather than showing a partial list.
    for (const SocketTable table : SOCKET_TABLES)
    {
        SocketTableCopy copy = copySocketTable(tables, table, buffer);
        if (!copy.ok)
        {
            return failure(ConnectionsReadStatus::Failed, std::move(copy.detail));
        }
        forEachSocketRow(table, buffer, keepTargets);
    }
    return result;
}

} // namespace Platform::Windows
