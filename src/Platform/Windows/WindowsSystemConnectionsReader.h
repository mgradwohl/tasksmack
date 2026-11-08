#pragma once

// Windows ISystemConnectionsReader (#3356): every TCP and UDP socket on the machine, with the process
// holding it, for the Connections tab. Each read() is:
//
//   1. GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) and GetExtendedUdpTable(UDP_TABLE_OWNER_PID), each
//      for AF_INET and AF_INET6 (WindowsSocketTables.h, shared with Process Details' reader, #1489). The
//      tables list every user's sockets with their owning PID, so no process is opened. A table that
//      can't be read is left out, as Linux leaves out a /proc/net table; the read fails only when none
//      can be read. A family Windows doesn't run (ERROR_NOT_SUPPORTED: IPv6 removed) reads as empty.
//   2. One NtQuerySystemInformation(SystemProcessInformation) snapshot for every holder's name and
//      creation time (the identity a ProcessTarget names), again without opening any process. A PID the
//      snapshot doesn't list (it exited meanwhile), or a failed snapshot, leaves the name and start time
//      empty; PID 0 (TIME_WAIT rows, which no process holds) is nobody's.
//   3. Sent/s and Received/s (#3079): TCP EStats data counters (GetPerTcp[6]ConnectionEStats) for each
//      ESTABLISHED connection, collection enabled once per connection (EStatsEnableTracker, #1418). Only
//      an elevated TaskSmack may enable it, so without administrator no EStats call is made and every
//      read says why in ratesUnavailable. An elevated read refused (a policy or driver) stops the reads
//      for good, saying so, as the process probe does (classifyEStatsProbe(), #1161).
//
// UDP sockets have no counters and stay UNCONN, as in Process Details. Nothing here changes or closes a
// connection (Close connection on Windows is #3358).
//
// Not thread-safe: the Connections tab's model calls read() under its sample mutex, on the tab's sampler
// thread. The constructor makes no call, so the reader can be built on the UI thread.

#ifdef _WIN32

#include "Platform/ISystemConnections.h"
#include "Platform/Windows/WindowsNtQuery.h"
#include "Platform/Windows/WindowsProcessProbeMath.h"
#include "Platform/Windows/WindowsSocketTables.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Platform::Windows
{

/// The calls WindowsSystemConnectionsReader makes: the system's, or a test's fakes.
struct SystemConnectionsFunctions
{
    // NOLINTBEGIN(misc-include-cleaner) - TCP_ESTATS_TYPE and the MIB rows come from iphlpapi.h's sub-headers
    using GetEStatsFn = ULONG(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
    using SetEStatsFn = ULONG(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);
    using GetEStats6Fn = ULONG(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
    using SetEStats6Fn = ULONG(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);
    // NOLINTEND(misc-include-cleaner)
    using IsElevatedFn = bool (*)();

    SocketTableFunctions tables;
    NtQuerySystemInformationFn ntQuerySystemInformation = Windows::ntQuerySystemInformation(); ///< Null: no names
    IsElevatedFn isElevated = nullptr; ///< Null: isProcessElevated(), asked on the first read
    GetEStatsFn getPerTcpConnectionEStats = &::GetPerTcpConnectionEStats;
    SetEStatsFn setPerTcpConnectionEStats = &::SetPerTcpConnectionEStats;
    GetEStats6Fn getPerTcp6ConnectionEStats = &::GetPerTcp6ConnectionEStats;
    SetEStats6Fn setPerTcp6ConnectionEStats = &::SetPerTcp6ConnectionEStats;
};

/// Why Sent/s and Received/s are missing on Windows, as SystemConnectionsRead::ratesUnavailable says it.
inline constexpr const char* RATES_NEED_ADMINISTRATOR =
    "Windows gives TCP connections' byte counts (TCP EStats) only to a program run as administrator. Run "
    "TaskSmack as administrator to see Sent/s and Received/s.";
inline constexpr const char* RATES_REFUSED = "Windows refused TCP EStats, which gives connections' byte counts, although TaskSmack runs as "
                                             "administrator: a policy or a driver on this machine blocks it.";

/// A process's name and creation time from the SystemProcessInformation snapshot.
struct SnapshotIdentity
{
    std::uint64_t createTime = 0; ///< FILETIME, as ProcessTarget::startTimeTicks; 0 for Idle and System
    std::string name;             ///< Its image name ("chrome.exe"), or empty
};

/// Every process in the SystemProcessInformation @p snapshot (only the bytes the kernel wrote), by PID. Every
/// entry and name is bounds-checked: a chain that runs past the bytes ends the walk, and a name outside them is
/// left empty.
[[nodiscard]] std::unordered_map<std::uint32_t, SnapshotIdentity> snapshotIdentities(std::span<const std::byte> snapshot);

class WindowsSystemConnectionsReader final : public ISystemConnectionsReader
{
  public:
    WindowsSystemConnectionsReader() = default;

    /// Test seam: make every call through @p api.
    explicit WindowsSystemConnectionsReader(const SystemConnectionsFunctions& api) : m_Api(api)
    {}

    [[nodiscard]] bool hasConnections() const override;
    [[nodiscard]] SystemConnectionsRead read() override;

  private:
    /// Whether TCP EStats is read, and if not, why.
    enum class RatesState : std::uint8_t
    {
        Unknown,    ///< Not yet asked: the first read decides
        Trying,     ///< Elevated: read, until a real sample says whether it works
        On,         ///< A real sample read connections' counters
        NeedsAdmin, ///< Not elevated: never read
        Refused,    ///< Elevated, but refused or broken on real connections: no longer read
    };

    /// Fills @p row's byte counters from TCP EStats, when it is ESTABLISHED and EStats is read.
    template<typename OwnerRowT> void readCounters(const OwnerRowT& ownerRow, SystemConnection& row);

    /// Updates m_RatesState from one read's tallies.
    void judgeRates(const EStatsSampleCounts& counts);

    SystemConnectionsFunctions m_Api;
    std::vector<std::byte> m_TableBuffer;
    std::vector<std::byte> m_SnapshotBuffer;
    RatesState m_RatesState = RatesState::Unknown;
    EStatsEnableTracker m_EStatsEnabled;
    EStatsSampleCounts m_Counts;           // this read's tallies
    std::size_t m_InconclusiveSamples = 0; // reads in a row whose EStats reads proved nothing (#1161)
    std::vector<EStatsConnectionRead> m_EStatsReads;
};

} // namespace Platform::Windows

#endif // _WIN32
