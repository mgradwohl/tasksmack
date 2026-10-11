#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ProcessTypes.h"
#include "WindowsProcessProbeMath.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 // NOLINT(cppcoreguidelines-macro-usage) - Windows platform requirement
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Windows headers must be in correct order:
// winsock2.h must come before windows.h
// windows.h must come before iphlpapi.h
// clang-format off
#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <iphlpapi.h>
#include <mstcpip.h>
// clang-format on

#include "WindowsHandles.h"
#include "WindowsNtQuery.h"

namespace Platform
{

/// The Win32/NT calls WindowsProcessProbe makes (#1718): the system's by default
/// (WindowsProcessProbe::systemFunctions()), fakes in tests, so the EStats network counters,
/// processor-group affinity and per-process detail reads, and their failures, can be tested
/// without elevation, a multi-group machine or a process that refuses them. Grouped by area.
/// Every member must be set, except the two noted as nullable.
struct WindowsProcessProbeFunctions
{
    /// iphlpapi.dll's TCP EStats exports. Any may be null: without the IPv4 pair network counters
    /// are off; without the IPv6 pair (#1100) only IPv4 connections are walked.
    struct TcpEStats
    {
        using GetFn = DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
        using SetFn = DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);
        using Get6Fn = DWORD(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
        using Set6Fn = DWORD(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);

        GetFn getPerTcpConnectionEStats = nullptr;
        SetFn setPerTcpConnectionEStats = nullptr;
        Get6Fn getPerTcp6ConnectionEStats = nullptr;
        Set6Fn setPerTcp6ConnectionEStats = nullptr;
    };

    struct Network
    {
        /// Resolve the EStats exports, loading iphlpapi.dll into @p ownedModule (freed with the
        /// probe) if no one has loaded it. Called only when elevated.
        TcpEStats (*loadTcpEStats)(Windows::UniqueModule& ownedModule) = nullptr;
        DWORD(WINAPI* getExtendedTcpTable)(PVOID, PDWORD, BOOL, ULONG, TCP_TABLE_CLASS, ULONG) = nullptr;
    } network;

    struct System
    {
        Windows::NtQuerySystemInformationFn ntQuerySystemInformation = nullptr; // Nullable: nothing is enumerated
        WORD(WINAPI* getMaximumProcessorGroupCount)() = nullptr;
        DWORD(WINAPI* getMaximumProcessorCount)(WORD) = nullptr;
        BOOL(WINAPI* getLogicalProcessorInformationEx)(LOGICAL_PROCESSOR_RELATIONSHIP,
                                                       PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
                                                       PDWORD) = nullptr;
        std::uint32_t (*windowsBuildNumber)() = nullptr; // 0 if unknown
    } system;

    struct Token
    {
        BOOL(WINAPI* openProcessToken)(HANDLE, DWORD, PHANDLE) = nullptr;
        BOOL(WINAPI* getTokenInformation)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD) = nullptr;
        BOOL(WINAPI* lookupAccountSidW)(LPCWSTR, PSID, LPWSTR, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE) = nullptr;
    } token;

    struct Process
    {
        HANDLE(WINAPI* openProcess)(DWORD, BOOL, DWORD) = nullptr;
        DWORD(WINAPI* getPriorityClass)(HANDLE) = nullptr;
        BOOL(WINAPI* queryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD) = nullptr;
        /// ntdll's NtQueryInformationProcess, its class passed as a ULONG. Nullable: status and
        /// command line are then not read.
        LONG(NTAPI* ntQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG) = nullptr;
        DWORD(WINAPI* getGuiResources)(HANDLE, DWORD) = nullptr;
        BOOL(WINAPI* getProcessAffinityMask)(HANDLE, PDWORD_PTR, PDWORD_PTR) = nullptr;
        BOOL(WINAPI* getProcessGroupAffinity)(HANDLE, PUSHORT, PUSHORT) = nullptr;
        DWORD(WINAPI* getProcessId)(HANDLE) = nullptr;
        HANDLE(WINAPI* openThread)(DWORD, BOOL, DWORD) = nullptr;
        DWORD(WINAPI* getProcessIdOfThread)(HANDLE) = nullptr;
        BOOL(WINAPI* getThreadGroupAffinity)(HANDLE, PGROUP_AFFINITY) = nullptr;
    } process;
};

/// Windows implementation of IProcessProbe.
/// Uses a single bulk NtQuerySystemInformation(SystemProcessInformation) snapshot per sample
/// (PIDs, names, CPU times, memory, I/O, handle/thread counts, thread states), plus TTL-cached
/// per-process details (owner, command line, publisher, classification, priority) refreshed via
/// short-lived handles.
class WindowsProcessProbe : public IProcessProbe
{
  public:
    WindowsProcessProbe();
    /// Make every Win32/NT call through @p functions rather than the system's (tests, #1718).
    explicit WindowsProcessProbe(const WindowsProcessProbeFunctions& functions);
    ~WindowsProcessProbe() override;

    /// The real calls: the Win32 APIs themselves, and ntdll's exports resolved once.
    [[nodiscard]] static WindowsProcessProbeFunctions systemFunctions();

    WindowsProcessProbe(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe& operator=(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe(WindowsProcessProbe&&) = delete;
    WindowsProcessProbe& operator=(WindowsProcessProbe&&) = delete;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;
    /// Keeps no per-connection traffic state between calls: a connection whose EStats read fails
    /// is reported unreadable and Domain keeps its baseline (#1256). It updates the set of
    /// connections whose EStats collection it already enabled (#1418, m_EStatsEnabled, under
    /// m_EStatsEnableMutex) and the one-time EStats verification (#1161, verifyEStats()), whose
    /// inconclusive-sample streak assumes one caller at a time: ProcessModel::refresh() is the only
    /// caller, under its sampling lock. Concurrent calls stay data-race-free (atomics and the
    /// mutex), but each would extend the streak.
    [[nodiscard]] SocketTrafficReading readSocketTraffic() const override;

  private:
    WindowsProcessProbeFunctions m_Fns; // Every Win32/NT call below goes through these (#1718)
    bool m_IsElevated = false;          // Process token elevation, queried once at construction (constant for the process lifetime)
    // The network flags can flip after construction when the first real sample proves EStats
    // unusable (#1161). readSocketTraffic()'s const EStats walk writes them and capabilities() may read
    // them from another thread, hence mutable atomics.
    mutable std::atomic<bool> m_HasNetworkCounters{false};
    mutable std::atomic<bool> m_NetworkCountersAccessDenied{
        false};                                        // True when EStats failed specifically due to access denied (privilege issue)
    mutable std::atomic<bool> m_EStatsVerified{false}; // A real sample has classified EStats (no more checks)
    std::chrono::milliseconds m_LightDetailTTL{1000};  // Default; tuned by total physical RAM in constructor
    std::chrono::milliseconds m_HeavyDetailTTL{5000};  // Default; tuned by total physical RAM in constructor
    Windows::UniqueModule m_IphlpModule;               // Non-null only when this class loaded iphlpapi.dll (freed on destruction)

    // Samples in a row whose established EStats reads were only NOT_FOUND / garbage (#1161)
    mutable std::atomic<std::size_t> m_EStatsInconclusiveSamples{0};

    // Connections whose EStats collection is already enabled, so each is enabled once rather than
    // every sample (#1418). readSocketTraffic() is const and documented safe to call concurrently,
    // so the tracker is mutable and guarded: one lock covers both table walks and the prune.
    mutable std::mutex m_EStatsEnableMutex;
    mutable EStatsEnableTracker m_EStatsEnabled; // Guarded by m_EStatsEnableMutex

    // Resolved by detectNetworkCounters(). A null IPv6 pair skips IPv6 (#1100).
    WindowsProcessProbeFunctions::TcpEStats m_EStats;

    struct DetailCacheKey
    {
        std::uint32_t pid = 0;
        std::uint64_t startTimeTicks = 0;

        [[nodiscard]] bool operator==(const DetailCacheKey& other) const noexcept = default;
    };

    struct DetailCacheKeyHash
    {
        [[nodiscard]] std::size_t operator()(const DetailCacheKey& key) const noexcept
        {
            const std::size_t pidHash = std::hash<std::uint32_t>{}(key.pid);
            const std::size_t startHash = std::hash<std::uint64_t>{}(key.startTimeTicks);
            return pidHash ^ (startHash + 0x9e3779b9U + (pidHash << 6U) + (pidHash >> 2U));
        }
    };

    struct DetailCacheEntry
    {
        std::string name; // Cached UTF-8 conversion of the image name (immutable per process)
        std::string user;
        std::string command;
        std::string status;
        std::string publisher;
        std::string processType;
        std::optional<std::int32_t> gdiObjectCount; // GetGuiResources (light TTL, #1156)
        // Slow-changing fields cached with light/heavy TTL to avoid redundant Win32 calls.
        CpuAffinity cpuAffinity;       // readCpuAffinity() (heavy TTL)
        std::int32_t nice = 0;         // GetPriorityClass → nice value (heavy TTL, or when the base priority changes)
        std::int32_t basePriority = 0; // Snapshot base priority last seen; a change re-reads the class (#1156)
        // GetPriorityClass, beside nice: Realtime and High share a nice bucket (#1280)
        PriorityClass priorityClass = PriorityClass::None;
        std::chrono::steady_clock::time_point nextLightRefresh;
        std::chrono::steady_clock::time_point nextHeavyRefresh;
        std::uint64_t generation = 0;
    };

    /// Refresh TTL-cached details for a single process (opens a handle only when a TTL expired, or
    /// the base priority changed: planDetailRefresh()).
    /// @param imageName Wide image name from the system snapshot (may be empty for pseudo-processes)
    /// @param basePriority The process's base priority from the system snapshot
    /// @param threadRecords The process's SYSTEM_THREAD_INFORMATION array from the snapshot (empty
    ///                      if it overran the entry); read only for a multi-group affinity.
    [[nodiscard]] bool getProcessDetails(uint32_t pid,
                                         ProcessCounters& counters,
                                         std::wstring_view imageName,
                                         std::int32_t basePriority,
                                         std::span<const std::byte> threadRecords);

    /// The process's CPU affinity, numbered as the per-core CPU figures are (#1247). One processor
    /// group: GetProcessAffinityMask, as ever. Several: GetProcessGroupAffinity's groups with
    /// their masks (groupMasksFromProcess(); the threads' GetThreadGroupAffinity only when that
    /// can't tell), mapped by cpuAffinityFromGroupMasks(). Empty if it can't be read, including
    /// when the processor topology couldn't be (affinityTopology()). Refreshes the groups' active
    /// masks when a heavy TTL has passed since the last read.
    [[nodiscard]] CpuAffinity readCpuAffinity(HANDLE hProcess, std::span<const std::byte> threadRecords);

    /// Read total system CPU time
    [[nodiscard]] static uint64_t readTotalCpuTime();
    // The highest total totalCpuTime() has returned, so it never steps backwards (#1303).
    mutable std::atomic<std::uint64_t> m_HighestTotalCpuTime{0};

    /// Calculate detail cache TTLs based on total physical RAM
    static void calculateDetailTTLsFromTotalRAM(std::chrono::milliseconds& lightTTL, std::chrono::milliseconds& heavyTTL) noexcept;

    /// Detect ETW/EStats availability for per-process network counters
    [[nodiscard]] bool detectNetworkCounters();

    /// Walk one address family's TCP table, appending each ESTABLISHED connection's EStats read to
    /// reads and tallying the Set/Get results into counts (debug line and #1161 detection).
    /// Returns false if the table could not be read this time: the walk is then incomplete
    /// (#1256). A family whose EStats functions are unavailable is not walked and returns true.
    /// @param enabled The enable tracker (#1418), passed in so the caller's m_EStatsEnableMutex
    ///                lock visibly covers every use of it.
    [[nodiscard]] bool
    collectTcp4Reads(EStatsEnableTracker& enabled, std::vector<EStatsConnectionRead>& reads, EStatsSampleCounts& counts) const;
    [[nodiscard]] bool
    collectTcp6Reads(EStatsEnableTracker& enabled, std::vector<EStatsConnectionRead>& reads, EStatsSampleCounts& counts) const;

    /// Log the periodic EStats debug line and, until a real sample has, decide whether EStats
    /// works (#1161). Returns false if this sample proved it unusable (network counters now off).
    [[nodiscard]] bool verifyEStats(const EStatsSampleCounts& counts) const;

    std::unordered_map<DetailCacheKey, DetailCacheEntry, DetailCacheKeyHash> m_DetailCache;
    std::uint64_t m_DetailCacheGeneration = 0;
    std::size_t m_LastEnumeratedProcessCount = 256;
    // Every processor group (#1247): maximum sizes read at construction, active masks re-read by
    // readCpuAffinity() each heavy TTL. More than one switches it to the group-aware reads; empty if
    // discovery failed (affinity then unreadable).
    std::vector<ProcessorGroupLayout> m_ProcessorGroups;
    bool m_ActiveMasksRead = false; // The last readActiveProcessorMasks() succeeded
    std::chrono::steady_clock::time_point m_NextActiveMasksRead;
    bool m_ThreadsMaySpanGroups = true;      // threadsMaySpanGroups() for this Windows build
    std::vector<std::byte> m_SnapshotBuffer; // Reused buffer for NtQuerySystemInformation snapshots
};

} // namespace Platform
