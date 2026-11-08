#pragma once

// The system's owner-PID socket tables (GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) and
// GetExtendedUdpTable(UDP_TABLE_OWNER_PID), each for AF_INET and AF_INET6), shared by the two readers
// that list sockets: Process Details' Connections section (WindowsProcessConnectionsReader, #1489),
// which keeps one process's rows, and the Connections tab (WindowsSystemConnectionsReader, #3356),
// which keeps every row.
//
//   - copySocketTable() copies one table into a buffer, growing it while the table outgrows it;
//   - forEachSocketRow() hands each row of a copied table to a callback, copied out of the buffer;
//   - toConnection() turns a row into a platform-neutral ProcessConnection.
//
// The calls go through a table of function pointers, the system's by default, so tests feed fixed
// tables, failures and tables that grow between the size query and the copy.

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

#include "Platform/IProcessConnections.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// The table calls: iphlpapi's, or a test's fakes.
struct SocketTableFunctions
{
    // NOLINTBEGIN(misc-include-cleaner) - TCP_TABLE_CLASS/UDP_TABLE_CLASS come from iprtrmib.h via iphlpapi.h
    using GetExtendedTcpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG);
    using GetExtendedUdpTableFn = DWORD(WINAPI*)(PVOID, PDWORD, BOOL, ULONG, UDP_TABLE_CLASS, ULONG);
    // NOLINTEND(misc-include-cleaner)

    GetExtendedTcpTableFn getExtendedTcpTable = &::GetExtendedTcpTable;
    GetExtendedUdpTableFn getExtendedUdpTable = &::GetExtendedUdpTable;
};

/// Tries at copying one socket table before giving up: a busy machine opens sockets between the call
/// that says how big the table is and the one that copies it.
inline constexpr int MAX_TABLE_ATTEMPTS = 4;

/// The size a socket table's buffer starts at: a desktop's whole TCP table fits in a few KiB.
inline constexpr std::size_t INITIAL_SOCKET_TABLE_BYTES = std::size_t{16} * 1024;

/// One of the four tables.
enum class SocketTable : std::uint8_t
{
    Tcp4,
    Tcp6,
    Udp4,
    Udp6,
};

/// What copySocketTable() made of one table.
struct SocketTableCopy
{
    bool ok = false;
    DWORD error = NO_ERROR; ///< The call's error when !ok (ERROR_INSUFFICIENT_BUFFER: it kept growing)
    std::string detail;     ///< What failed, for the user, when !ok
};

/// Copies @p table into @p buffer (made at least INITIAL_SOCKET_TABLE_BYTES long, and never shrunk), growing it
/// while the table outgrows it, at most MAX_TABLE_ATTEMPTS times.
[[nodiscard]] SocketTableCopy copySocketTable(const SocketTableFunctions& api, SocketTable table, std::vector<std::byte>& buffer);

/// Calls @p onRow with each row of @p table (a copied MIB_*TABLE_OWNER_PID: a row count, then the rows). A
/// count larger than the buffer holds is clamped to what it holds. Rows are copied out rather than read in place.
template<typename TableT, typename RowT, typename OnRow> void forEachSocketRow(std::span<const std::byte> table, OnRow& onRow)
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
        onRow(row);
    }
}

/// forEachSocketRow() for a copy of @p table: @p onRow takes that table's row type (MIB_TCPROW_OWNER_PID,
/// MIB_TCP6ROW_OWNER_PID, MIB_UDPROW_OWNER_PID or MIB_UDP6ROW_OWNER_PID), so a generic lambda serves all four.
template<typename OnRow> void forEachSocketRow(SocketTable table, std::span<const std::byte> bytes, OnRow& onRow)
{
    switch (table)
    {
    case SocketTable::Tcp4:
        forEachSocketRow<MIB_TCPTABLE_OWNER_PID, MIB_TCPROW_OWNER_PID>(bytes, onRow);
        break;
    case SocketTable::Tcp6:
        forEachSocketRow<MIB_TCP6TABLE_OWNER_PID, MIB_TCP6ROW_OWNER_PID>(bytes, onRow);
        break;
    case SocketTable::Udp4:
        forEachSocketRow<MIB_UDPTABLE_OWNER_PID, MIB_UDPROW_OWNER_PID>(bytes, onRow);
        break;
    case SocketTable::Udp6:
        forEachSocketRow<MIB_UDP6TABLE_OWNER_PID, MIB_UDP6ROW_OWNER_PID>(bytes, onRow);
        break;
    }
}

/// The four tables, in the order the readers copy them.
inline constexpr std::array<SocketTable, 4> SOCKET_TABLES{SocketTable::Tcp4, SocketTable::Tcp6, SocketTable::Udp4, SocketTable::Udp6};

/// A MIB_TCP_STATE (MIB_TCP_STATE_CLOSED .. MIB_TCP_STATE_DELETE_TCB) as a Platform::ConnectionState.
/// DELETE_TCB (a control block being torn down) reads as Closed; anything else unknown as Unknown.
/// That includes 0, which is no MIB_TCP_STATE but which Windows build 26300 reports for many rows,
/// listeners among them (#3257): such a row is shown as UNKNOWN, never guessed as Closed or Listen.
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

} // namespace Platform::Windows

#endif // _WIN32
