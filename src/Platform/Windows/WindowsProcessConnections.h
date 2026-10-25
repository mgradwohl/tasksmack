#pragma once

// Windows IProcessConnectionsReader (#1489): the selected process's TCP and UDP sockets, netstat-style,
// from the system's owner-PID socket tables.
//
//   1. OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION), and its creation time (GetProcessTimes) checked
//      against the target's, so a reused PID reads as ProcessExited. The handle stays open for the rest
//      of the read: it pins the PID, so the process checked is the one whose rows are kept.
//   2. GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) and GetExtendedUdpTable(UDP_TABLE_OWNER_PID), each
//      for AF_INET and AF_INET6, kept only for rows whose owning PID is the target's.
//
// Unlike Linux, the tables list every user's sockets with their owning PID, so the sockets themselves
// are never refused. Only step 1 can be: a process TaskSmack may not open even for limited query
// (csrss, smss and other protected SYSTEM processes, from an unelevated TaskSmack) reads as
// PermissionDenied, since its identity cannot be confirmed and the rows of a PID that may have been
// reused are not shown.
//
// Read-only: nothing here changes or closes a connection.
//
// The calls it makes are a table of function pointers, the system's by default, so tests feed it fixed
// tables, denials and tables that grow between the size query and the copy
// (test_WindowsProcessConnections.cpp).

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
#include "Platform/IProcessConnections.h"

#include <cstdint>

namespace Platform::Windows
{

/// The calls WindowsProcessConnectionsReader makes: iphlpapi's and kernel32's, or a test's fakes.
struct ProcessConnectionsFunctions
{
    // NOLINTBEGIN(misc-include-cleaner) - TCP_TABLE_CLASS/UDP_TABLE_CLASS come from iprtrmib.h via iphlpapi.h
    using GetExtendedTcpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
    using GetExtendedUdpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, UDP_TABLE_CLASS, ULONG);
    // NOLINTEND(misc-include-cleaner)
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using GetExitCodeProcessFn = BOOL(WINAPI*)(HANDLE, LPDWORD);

    GetExtendedTcpTableFn getExtendedTcpTable = &::GetExtendedTcpTable;
    GetExtendedUdpTableFn getExtendedUdpTable = &::GetExtendedUdpTable;
    OpenProcessFn openProcess = &::OpenProcess;
    CloseHandleFn closeHandle = &::CloseHandle;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    GetExitCodeProcessFn getExitCodeProcess = &::GetExitCodeProcess;
};

/// Tries at copying one socket table before giving up: a busy machine opens sockets between the call
/// that says how big the table is and the one that copies it.
inline constexpr int MAX_TABLE_ATTEMPTS = 4;

/// A MIB_TCP_STATE (MIB_TCP_STATE_CLOSED .. MIB_TCP_STATE_DELETE_TCB) as a Platform::ConnectionState.
/// DELETE_TCB (a control block being torn down) reads as Closed; anything else unknown as Unknown.
[[nodiscard]] ConnectionState toConnectionState(DWORD mibState) noexcept;

/// A table row's port: the port in network byte order in the DWORD's low 16 bits, as a host-order number.
[[nodiscard]] std::uint16_t toHostPort(DWORD tablePort) noexcept;

/// One table row as a ProcessConnection. Addresses are copied in network byte order (an IPv4 address's
/// DWORD already is, in memory). A UDP row has no remote end and no state: remote all zero, Closed
/// (bound but unconnected; the tables do not say whether a UDP socket called connect()).
[[nodiscard]] ProcessConnection toConnection(const MIB_TCPROW_OWNER_PID& row) noexcept;
[[nodiscard]] ProcessConnection toConnection(const MIB_TCP6ROW_OWNER_PID& row) noexcept;
[[nodiscard]] ProcessConnection toConnection(const MIB_UDPROW_OWNER_PID& row) noexcept;
[[nodiscard]] ProcessConnection toConnection(const MIB_UDP6ROW_OWNER_PID& row) noexcept;

/// Stateless: each read opens, reads and closes on its own, so reads may run on any thread.
class WindowsProcessConnectionsReader final : public IProcessConnectionsReader
{
  public:
    WindowsProcessConnectionsReader() = default;

    /// Test seam: make every call through @p api.
    explicit WindowsProcessConnectionsReader(const ProcessConnectionsFunctions& api) : m_Api(api)
    {}

    [[nodiscard]] bool hasConnections() const override;
    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& target) override;

  private:
    ProcessConnectionsFunctions m_Api;
};

} // namespace Platform::Windows

#endif // _WIN32
