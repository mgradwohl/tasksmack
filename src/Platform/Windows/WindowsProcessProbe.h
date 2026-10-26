#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IUdpTrafficCollector.h"
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
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

#include "WindowsEnergyMeter.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"

namespace Platform
{

/// The Win32/NT calls WindowsProcessProbe makes (#1718): the system's by default
/// (WindowsProcessProbe::systemFunctions()), fakes in tests, so the EStats network counters,
/// processor-group affinity and per-process detail reads, and their failures, can be tested
/// without elevation, a multi-group machine or a process that refuses them. Grouped by area.
/// Every member must be set, except those noted as nullable.
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
        /// The detail caches' clock (steady_clock::now()), so tests can hold time still or move it on
        /// rather than depend on how fast they run (#2054).
        std::chrono::steady_clock::time_point (*steadyNow)() = nullptr;
        /// The machine Windows runs as (Windows::nativeMachine()), read once at construction, against which
        /// a process's architecture is emulated or not (#2394). Nullable: every process's is then named alone.
        std::uint16_t (*nativeMachine)() = nullptr;
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
        /// GetProcessInformation: the power-throttling state behind Efficiency Mode (#2312).
        BOOL(WINAPI* getProcessInformation)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD) = nullptr;
        DWORD(WINAPI* getGuiResources)(HANDLE, DWORD) = nullptr;
        BOOL(WINAPI* getProcessAffinityMask)(HANDLE, PDWORD_PTR, PDWORD_PTR) = nullptr;
        BOOL(WINAPI* getProcessGroupAffinity)(HANDLE, PUSHORT, PUSHORT) = nullptr;
        DWORD(WINAPI* getProcessId)(HANDLE) = nullptr;
        HANDLE(WINAPI* openThread)(DWORD, BOOL, DWORD) = nullptr;
        DWORD(WINAPI* getProcessIdOfThread)(HANDLE) = nullptr;
        BOOL(WINAPI* getThreadGroupAffinity)(HANDLE, PGROUP_AFFINITY) = nullptr;
        /// The machine a process runs as (Windows::processMachine(): ProcessMachineTypeInfo, else
        /// IsWow64Process2), read once per process for its architecture (#2394). Nullable: not read.
        std::uint16_t (*processMachine)(HANDLE) = nullptr;
    } process;

    /// The EMI energy meter device calls behind per-process power (#2587), opened on the probe's
    /// one-shot opener thread. Nullable (nullopt): no energy meter is looked for, and power is not
    /// claimed, so a test's fake probe starts no thread and touches no device.
    std::optional<Windows::EnergyMeterFunctions> energyMeter = std::nullopt;
};

/// The reads WindowsProcessProbe makes for one process on its heavy detail cadence, each callable
/// alone so the heavy pass can be timed read by read (benchmarks, #2982). The probe makes these same
/// calls; each takes a handle opened with PROCESS_QUERY_LIMITED_INFORMATION and returns empty when
/// its read fails.
namespace WindowsProcessDetailReads
{
/// The account that owns the process: its token's user SID, named by LookupAccountSidW.
[[nodiscard]] std::string owner(const WindowsProcessProbeFunctions::Token& fns, HANDLE hProcess);
/// The same, as the probe reads it: a SID @p names named less than its TTL before @p now isn't
/// looked up again, and a lookup that succeeds is stored in it.
[[nodiscard]] std::string
owner(const WindowsProcessProbeFunctions::Token& fns, HANDLE hProcess, AccountNameCache& names, std::chrono::steady_clock::time_point now);
/// The executable's full path (QueryFullProcessImageNameW).
[[nodiscard]] std::wstring imagePath(const WindowsProcessProbeFunctions::Process& fns, HANDLE hProcess);
/// The command line, from the process's PEB (NtQueryInformationProcess).
[[nodiscard]] std::string commandLine(const WindowsProcessProbeFunctions::Process& fns, HANDLE hProcess);
/// The CompanyName from the executable's version info, cached by path for the life of the program.
[[nodiscard]] std::string publisher(const std::wstring& imagePath);
/// "App", "Background Process" or "Windows Process" (hQuery may be null: the USER objects aren't counted).
[[nodiscard]] std::string_view
processType(const WindowsProcessProbeFunctions::Process& fns, HANDLE hQuery, DWORD pid, std::string_view imagePath);
} // namespace WindowsProcessDetailReads

/// Windows implementation of IProcessProbe.
/// Uses a single bulk NtQuerySystemInformation(SystemProcessInformation) snapshot per sample
/// (PIDs, names, CPU times, memory, I/O, handle/thread counts, thread states), plus TTL-cached
/// per-process details (owner, command line, publisher, classification, priority) refreshed via
/// short-lived handles.
/// Per-process power (#2587) is the CPU packages' energy from the EMI energy meters
/// (WindowsEnergyMeter.h), read raw each sample by readPackageEnergy() and shared out by CPU time
/// in Domain, as Linux's RAPL counter is. The meters are enumerated and opened once, on the
/// ts-emi-open worker started at construction (SetupAPI can stall on a slow ACPI call, so never on
/// the thread that builds the probe); hasPowerUsage turns true once it finds a package channel, and
/// stays false where there is none.
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
    /// While the opt-in UDP collector (#1492) delivers, a complete reading also carries each
    /// process's UDP bytes as one synthetic connection (EtwNet::appendUdpSamples()).
    [[nodiscard]] SocketTrafficReading readSocketTraffic() const override;

    /// The CPU packages' cumulative energy from the EMI meters (#2587), with the busy CPU ticks read
    /// alongside, or nullopt while no package channel is open (none found, or the opener still running).
    /// A read that fails (a meter gone) is a reading with no energy, so the interval reads as 0 W and
    /// the baseline restarts, as on Linux; it is logged once, not per sample.
    [[nodiscard]] std::optional<PackageEnergyReading> readPackageEnergy() const override;

    /// The opt-in UDP collector (#1492), which App starts and stops; may be null.
    void setUdpTrafficCollector(const std::shared_ptr<IUdpTrafficCollector>& collector) override;

  private:
    /// The UDP collector, if one is set (#1492).
    [[nodiscard]] std::shared_ptr<IUdpTrafficCollector> udpCollector() const;

    WindowsProcessProbeFunctions m_Fns; // Every Win32/NT call below goes through these (#1718)
    bool m_IsElevated = false;          // Process token elevation, queried once at construction (constant for the process lifetime)
    std::uint16_t m_NativeMachine = 0;  // The machine Windows runs as (IMAGE_FILE_MACHINE_*), read once at construction (#2394)
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

    // The opt-in UDP collector (#1492): set from the App thread, read on the sampler's.
    mutable std::mutex m_UdpCollectorMutex;
    std::shared_ptr<IUdpTrafficCollector> m_UdpCollector; // Guarded by m_UdpCollectorMutex

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
        std::string architecture;                   // MachineMath::windowsArchitectureLabel() (#2394): read once, it never changes
        bool architectureRead = false;              // Whether architecture above was read (with the first handle opened)
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
    /// The system-wide CPU time spent running processes (kernel less idle, plus user), in FILETIME
    /// ticks: the energy attribution's share denominator (#2587). nullopt if GetSystemTimes failed.
    [[nodiscard]] static std::optional<std::uint64_t> readBusyCpuTicks();
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
    AccountNameCache m_AccountNames;         // Owners' account names by SID, for the heavy reads (#2982)

    // The EMI package energy meters (#2587). The opener thread stores the opened meter under the
    // mutex and then raises m_HasPackageEnergy; readPackageEnergy() (const, on the sampler thread)
    // reads under the same mutex, so both are mutable. capabilities() reads only the atomic.
    mutable std::mutex m_EnergyMeterMutex;
    mutable Windows::EnergyMeter m_EnergyMeter;           // Guarded by m_EnergyMeterMutex
    mutable std::vector<std::uint64_t> m_PackageEnergyUj; // Guarded by m_EnergyMeterMutex; reused scratch
    mutable std::atomic<bool> m_HasPackageEnergy{false};
    mutable std::atomic<bool> m_PackageEnergyReadFailed{false}; // A failed measurement read was logged
    std::jthread m_EnergyMeterOpener;                           // Last: joined first, while the meter and the mutex are alive
};

} // namespace Platform
