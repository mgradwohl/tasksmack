#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/IProcessProbe.h"
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
#include <optional>
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

namespace Platform
{

/// Windows implementation of IProcessProbe.
/// Uses a single bulk NtQuerySystemInformation(SystemProcessInformation) snapshot per sample
/// (PIDs, names, CPU times, memory, I/O, handle/thread counts, thread states), plus TTL-cached
/// per-process details (owner, command line, publisher, classification, priority) refreshed via
/// short-lived handles.
class WindowsProcessProbe : public IProcessProbe
{
  public:
    WindowsProcessProbe();
    ~WindowsProcessProbe() override;

    WindowsProcessProbe(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe& operator=(const WindowsProcessProbe&) = delete;
    WindowsProcessProbe(WindowsProcessProbe&&) = delete;
    WindowsProcessProbe& operator=(WindowsProcessProbe&&) = delete;

    [[nodiscard]] std::vector<ProcessCounters> enumerate() override;
    [[nodiscard]] ProcessCapabilities capabilities() const override;
    [[nodiscard]] uint64_t totalCpuTime() const override;
    [[nodiscard]] long ticksPerSecond() const override;
    [[nodiscard]] uint64_t systemTotalMemory() const override;
    /// Keeps no per-connection state between calls: a connection whose EStats read fails is
    /// reported unreadable and Domain keeps its baseline (#1256). The only state it updates is the
    /// one-time EStats verification (#1161, verifyEStats()), whose inconclusive-sample streak
    /// assumes one caller at a time: ProcessModel::refresh() is the only caller, under its sampling
    /// lock. Concurrent calls stay data-race-free (atomics), but each would extend the streak.
    [[nodiscard]] SocketTrafficReading readSocketTraffic() const override;

  private:
    bool m_IsElevated = false; // Process token elevation, queried once at construction (constant for the process lifetime)
    // The network flags can flip after construction when the first real sample proves EStats
    // unusable (#1161). readSocketTraffic()'s const EStats walk writes them and capabilities() may read
    // them from another thread, hence mutable atomics.
    mutable std::atomic<bool> m_HasNetworkCounters{false};
    mutable std::atomic<bool> m_NetworkCountersAccessDenied{
        false};                                        // True when EStats failed specifically due to access denied (privilege issue)
    mutable std::atomic<bool> m_EStatsVerified{false}; // A real sample has classified EStats (no more checks)
    std::chrono::milliseconds m_LightDetailTTL{1000};  // Default; tuned by total physical RAM in constructor
    std::chrono::milliseconds m_HeavyDetailTTL{5000};  // Default; tuned by total physical RAM in constructor
    HMODULE m_IphlpModule = nullptr;                   // Non-null only when loaded by this class (must be freed in destructor)

    // Samples in a row whose established EStats reads were only NOT_FOUND / garbage (#1161)
    mutable std::atomic<std::size_t> m_EStatsInconclusiveSamples{0};

    // EStats function signatures
    using GetPerTcpConnectionEStatsFn =
        DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
    using SetPerTcpConnectionEStatsFn = DWORD(WINAPI*)(PMIB_TCPROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);
    // IPv6 twins (#1100): same shape, MIB_TCP6ROW instead of MIB_TCPROW
    using GetPerTcp6ConnectionEStatsFn =
        DWORD(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG, PUCHAR, ULONG, ULONG);
    using SetPerTcp6ConnectionEStatsFn = DWORD(WINAPI*)(PMIB_TCP6ROW, TCP_ESTATS_TYPE, PUCHAR, ULONG, ULONG, ULONG);

    GetPerTcpConnectionEStatsFn m_GetPerTcpConnectionEStats = nullptr;
    SetPerTcpConnectionEStatsFn m_SetPerTcpConnectionEStats = nullptr;
    GetPerTcp6ConnectionEStatsFn m_GetPerTcp6ConnectionEStats = nullptr; // Null if unresolved: IPv6 is then skipped
    SetPerTcp6ConnectionEStatsFn m_SetPerTcp6ConnectionEStats = nullptr;

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
        CpuAffinity cpuAffinity;       // GetProcessAffinityMask (heavy TTL)
        std::int32_t nice = 0;         // GetPriorityClass → nice value (heavy TTL, or when the base priority changes)
        std::int32_t basePriority = 0; // Snapshot base priority last seen; a change re-reads the class (#1156)
        std::chrono::steady_clock::time_point nextLightRefresh;
        std::chrono::steady_clock::time_point nextHeavyRefresh;
        std::uint64_t generation = 0;
    };

    /// Refresh TTL-cached details for a single process (opens a handle only when a TTL expired, or
    /// the base priority changed: planDetailRefresh()).
    /// @param imageName Wide image name from the system snapshot (may be empty for pseudo-processes)
    /// @param basePriority The process's base priority from the system snapshot
    [[nodiscard]] bool getProcessDetails(uint32_t pid, ProcessCounters& counters, std::wstring_view imageName, std::int32_t basePriority);

    /// Read total system CPU time
    [[nodiscard]] static uint64_t readTotalCpuTime();

    /// Calculate detail cache TTLs based on total physical RAM
    static void calculateDetailTTLsFromTotalRAM(std::chrono::milliseconds& lightTTL, std::chrono::milliseconds& heavyTTL) noexcept;

    /// Detect ETW/EStats availability for per-process network counters
    [[nodiscard]] bool detectNetworkCounters();

    /// Walk one address family's TCP table, appending each ESTABLISHED connection's EStats read to
    /// reads and tallying the Set/Get results into counts (debug line and #1161 detection).
    /// Returns false if the table could not be read this time: the walk is then incomplete
    /// (#1256). A family whose EStats functions are unavailable is not walked and returns true.
    [[nodiscard]] bool collectTcp4Reads(std::vector<EStatsConnectionRead>& reads, EStatsSampleCounts& counts) const;
    [[nodiscard]] bool collectTcp6Reads(std::vector<EStatsConnectionRead>& reads, EStatsSampleCounts& counts) const;

    /// Log the periodic EStats debug line and, until a real sample has, decide whether EStats
    /// works (#1161). Returns false if this sample proved it unusable (network counters now off).
    [[nodiscard]] bool verifyEStats(const EStatsSampleCounts& counts) const;

    std::unordered_map<DetailCacheKey, DetailCacheEntry, DetailCacheKeyHash> m_DetailCache;
    std::uint64_t m_DetailCacheGeneration = 0;
    std::size_t m_LastEnumeratedProcessCount = 256;
    std::vector<std::byte> m_SnapshotBuffer; // Reused buffer for NtQuerySystemInformation snapshots
};

} // namespace Platform
