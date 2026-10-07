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

#include "WindowsProcessProbeMath.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <vector>

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

/// The endpoints estatsConnectionKey() identifies an IPv4 connection by (#1256). The addresses are
/// stored in network byte order, so their in-memory bytes are copied as they are.
[[nodiscard]] inline TcpConnectionEndpoints toConnectionEndpoints(const MIB_TCPROW_OWNER_PID& ownerRow) noexcept
{
    TcpConnectionEndpoints endpoints;
    endpoints.family = TcpAddressFamily::IPv4;
    endpoints.localPort = ownerRow.dwLocalPort;
    endpoints.remotePort = ownerRow.dwRemotePort;
    const auto localAddr = std::bit_cast<std::array<std::uint8_t, 4>>(ownerRow.dwLocalAddr);
    const auto remoteAddr = std::bit_cast<std::array<std::uint8_t, 4>>(ownerRow.dwRemoteAddr);
    std::ranges::copy(localAddr, endpoints.localAddr.begin());
    std::ranges::copy(remoteAddr, endpoints.remoteAddr.begin());
    return endpoints;
}

/// IPv6 twin of the above (#1256): 16-byte addresses plus each end's scope id.
[[nodiscard]] inline TcpConnectionEndpoints toConnectionEndpoints(const MIB_TCP6ROW_OWNER_PID& ownerRow) noexcept
{
    return TcpConnectionEndpoints{
        .family = TcpAddressFamily::IPv6,
        .localAddr = std::bit_cast<std::array<std::uint8_t, 16>>(ownerRow.ucLocalAddr),
        .localScopeId = ownerRow.dwLocalScopeId,
        .localPort = ownerRow.dwLocalPort,
        .remoteAddr = std::bit_cast<std::array<std::uint8_t, 16>>(ownerRow.ucRemoteAddr),
        .remoteScopeId = ownerRow.dwRemoteScopeId,
        .remotePort = ownerRow.dwRemotePort,
    };
}

// NOLINTBEGIN(misc-include-cleaner) - TCP_ESTATS_* come from tcpestats.h via the iphlpapi.h umbrella
/// Read EStats for one ESTABLISHED connection, enabling collection first only if @p enabled has
/// no remembered enable for it (#1418), hand the result to the shared recordEStatsRow() tally,
/// and append the read to @p reads (#1256). RowT is MIB_TCPROW or MIB_TCP6ROW (#1100); the Set/Get
/// functions are the matching IPv4 or IPv6 pair. @p setFn may be null (the read is then tried
/// without an enable, which works if another process enabled collection).
template<typename RowT, typename SetFn, typename GetFn>
void readEStatsRow(RowT& row,
                   std::uint64_t key,
                   std::uint32_t pid,
                   std::uint32_t state,
                   SetFn setFn,
                   GetFn getFn,
                   EStatsEnableTracker& enabled,
                   std::vector<EStatsConnectionRead>& reads,
                   EStatsSampleCounts& counts)
{
    // Enable collection (requires admin, may fail) unless an earlier sample already did. A
    // known connection's read is tallied with no enable status, as when setFn is null.
    std::optional<std::uint32_t> enableStatus;
    if (setFn != nullptr)
    {
        if (enabled.needsEnable(key))
        {
            TCP_ESTATS_DATA_RW_v0 rw{};
            rw.EnableCollection = TRUE;
            enableStatus = setFn(&row, TcpConnectionEstatsData, reinterpret_cast<PUCHAR>(&rw), 0, sizeof(rw), 0);
        }
        else if (state == TCP_STATE_ESTABLISHED)
        {
            ++counts.alreadyEnabled;
        }
    }

    // Read the stats (may work even if enable failed, if another process enabled it)
    TCP_ESTATS_DATA_ROD_v0 rod{};
    const DWORD readStatus =
        getFn(&row, TcpConnectionEstatsData, nullptr, 0, 0, nullptr, 0, 0, reinterpret_cast<PUCHAR>(&rod), 0, sizeof(rod));

    const EStatsRowOutcome outcome = recordEStatsRow(counts, state, enableStatus, readStatus, rod.DataBytesOut, rod.DataBytesIn);
    enabled.record(key, enableStatus, outcome, rod.DataBytesOut > 0 || rod.DataBytesIn > 0);
    reads.push_back(
        EStatsConnectionRead{.key = key, .pid = pid, .outcome = outcome, .bytesReceived = rod.DataBytesIn, .bytesSent = rod.DataBytesOut});
}
// NOLINTEND(misc-include-cleaner)

} // namespace Platform

#endif // _WIN32
