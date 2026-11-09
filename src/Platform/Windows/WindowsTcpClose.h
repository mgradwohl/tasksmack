#pragma once

// Close connection on Windows (#3358): an IPv4 TCP connection reset with SetTcpEntry(MIB_TCP_STATE_DELETE_TCB),
// as TCPView and Process Explorer close one. Windows deletes the connection's control block and sends the peer a
// reset; the process holding the socket gets an error on its next use of it.
//
// The connection is named by its endpoints, since Windows has no socket inode. Just before acting, the IPv4 TCP
// owner-PID table is read again (WindowsSocketTables) and the connection must still be there, held by the PID the
// user confirmed, and not listening; WindowsProcessActions holds the verified process handle across this, so the
// PID can't pass to another process in between. SetTcpEntry has no IPv6 twin, so an IPv6 connection can't be
// closed. Setting a connection's state needs an administrator: a refusal is permissionDenied, so
// BrokeredProcessActions retries it through the elevated helper.
//
// The calls go through a table, so tests feed fixed tables and SetTcpEntry results without touching the network.

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // NOLINT(cppcoreguidelines-macro-usage) - Windows platform requirement
#endif

// NOLINTBEGIN(misc-include-cleaner) - Windows umbrella headers; symbols come from implementation
// sub-headers that cannot be included individually without breaking the required include order.
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on
// NOLINTEND(misc-include-cleaner)

#include "Platform/IProcessActions.h"
#include "WindowsSocketTables.h"

#include <cstdint>

namespace Platform::Windows
{

/// The calls closeTcpConnectionOf() makes: the table read and SetTcpEntry, iphlpapi's or a test's.
struct TcpCloseFunctions
{
    SocketTableFunctions tables{};
    // NOLINTNEXTLINE(misc-include-cleaner) - PMIB_TCPROW comes from tcpmib.h via iphlpapi.h
    DWORD(WINAPI* setTcpEntry)(PMIB_TCPROW row) = nullptr; ///< Null: no Close connection (canCloseConnections false)
};

/// The real calls.
[[nodiscard]] TcpCloseFunctions realTcpCloseFunctions() noexcept;

/// SetTcpEntry's answer to a caller that isn't an administrator, besides ERROR_ACCESS_DENIED: Windows has long
/// returned ERROR_MR_MID_NOT_FOUND (317) for it, an error code the API reuses.
inline constexpr DWORD SET_TCP_ENTRY_NOT_ADMINISTRATOR = 317;

/// What the refusal for want of administrator rights says.
inline constexpr const char* CLOSE_CONNECTION_NEEDS_ADMINISTRATOR = "Closing a connection needs administrator rights";

/// Resets the IPv4 TCP connection @p connection that PID @p pid holds, through @p fns: the table read again, the
/// row matched by both endpoints, then SetTcpEntry on it with MIB_TCP_STATE_DELETE_TCB. Refused, with nothing
/// set, for a connection with no remote end, one no longer in the table or held by another PID, a listener, and
/// one already closed or in TIME_WAIT. Access denied (or 317) is permissionDenied. The caller has checked the
/// PID is the process the user confirmed, and holds a handle to it.
[[nodiscard]] ProcessActionResult
closeTcpConnectionOf(const TcpCloseFunctions& fns, std::int32_t pid, const TcpConnectionToClose& connection);

} // namespace Platform::Windows

#endif // _WIN32
