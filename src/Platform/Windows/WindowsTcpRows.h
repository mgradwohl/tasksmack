#pragma once

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

#include <bit>

namespace Platform
{

/// Convert a GetExtendedTcpTable(AF_INET, TCP_TABLE_OWNER_PID_ALL) row into the MIB_TCPROW that
/// Set/GetPerTcpConnectionEStats identify the connection by (#1100). Addresses and ports are
/// copied verbatim: the table stores each port in network byte order in the low 16 bits of a
/// DWORD, which is exactly what MIB_TCPROW expects, so no byte swapping happens here.
[[nodiscard]] inline MIB_TCPROW toTcpRow(const MIB_TCPROW_OWNER_PID& ownerRow) noexcept
{
    MIB_TCPROW row{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access) - Windows API requires union access
    row.dwState = ownerRow.dwState;
    row.dwLocalAddr = ownerRow.dwLocalAddr;
    row.dwLocalPort = ownerRow.dwLocalPort;
    row.dwRemoteAddr = ownerRow.dwRemoteAddr;
    row.dwRemotePort = ownerRow.dwRemotePort;
    return row;
}

/// IPv6 twin of toTcpRow() for Set/GetPerTcp6ConnectionEStats (#1100). MIB_TCP6ROW identifies the
/// connection by address + scope id + port on both ends; the owner-PID row stores the addresses
/// as raw 16-byte arrays (bit_cast to IN6_ADDR) and the ports, like IPv4, in network byte order,
/// copied verbatim.
[[nodiscard]] inline MIB_TCP6ROW toTcp6Row(const MIB_TCP6ROW_OWNER_PID& ownerRow) noexcept
{
    return MIB_TCP6ROW{
        .State = static_cast<MIB_TCP_STATE>(ownerRow.dwState),
        .LocalAddr = std::bit_cast<IN6_ADDR>(ownerRow.ucLocalAddr),
        .dwLocalScopeId = ownerRow.dwLocalScopeId,
        .dwLocalPort = ownerRow.dwLocalPort,
        .RemoteAddr = std::bit_cast<IN6_ADDR>(ownerRow.ucRemoteAddr),
        .dwRemoteScopeId = ownerRow.dwRemoteScopeId,
        .dwRemotePort = ownerRow.dwRemotePort,
    };
}

} // namespace Platform

#endif // _WIN32
