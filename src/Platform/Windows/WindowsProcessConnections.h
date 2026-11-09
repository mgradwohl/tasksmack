#pragma once

// Windows IProcessConnectionsReader (#1489): the selected process's TCP and UDP sockets, netstat-style,
// from the system's owner-PID socket tables.
//
//   1. OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION), and its creation time (GetProcessTimes) checked
//      against the target's, so a reused PID reads as ProcessExited. The handle stays open for the rest
//      of the read: it pins the PID, so the process checked is the one whose rows are kept.
//   2. GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) and GetExtendedUdpTable(UDP_TABLE_OWNER_PID), each
//      for AF_INET and AF_INET6 (WindowsSocketTables.h, shared with the Connections tab's reader, #3356),
//      kept only for rows whose owning PID is the target's.
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
#include "WindowsSocketTables.h"

namespace Platform::Windows
{

/// The calls WindowsProcessConnectionsReader makes: iphlpapi's and kernel32's, or a test's fakes.
struct ProcessConnectionsFunctions
{
    using GetExtendedTcpTableFn = SocketTableFunctions::GetExtendedTcpTableFn;
    using GetExtendedUdpTableFn = SocketTableFunctions::GetExtendedUdpTableFn;
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
