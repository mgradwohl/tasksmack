#pragma once

#include "Platform/CpuAffinity.h"
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Platform
{

/// TTLs for the light/heavy detail-refresh caches, tuned by calculateDetailTTLsFromTotalRAMBytes().
struct DetailCacheTTLs
{
    std::chrono::milliseconds light{0};
    std::chrono::milliseconds heavy{0};
};

/// Pure RAM-tier decision logic extracted from WindowsProcessProbe::calculateDetailTTLsFromTotalRAM()
/// so its tier thresholds can be unit tested with fabricated byte counts - the real function's
/// GlobalMemoryStatusEx() call reports whatever RAM this machine happens to have, so most tiers
/// never execute in CI. Heuristic: systems with abundant RAM can afford frequent detail refreshes
/// (lower latency), while memory-constrained systems should cache longer to reduce enumeration
/// overhead.
/// @param totalPhysicalBytes MEMORYSTATUSEX::ullTotalPhys (total physical RAM in bytes)
[[nodiscard]] inline DetailCacheTTLs calculateDetailTTLsFromTotalRAMBytes(std::uint64_t totalPhysicalBytes) noexcept
{
    // Tier thresholds and corresponding TTLs (based on total physical RAM):
    // - < 2 GB total: Aggressive caching (preserve CPU/memory on low-end systems)
    // - 2-4 GB: Conservative refresh
    // - 4-8 GB: Balanced refresh
    // - 8-16 GB: Slightly more frequent, but still avoids per-frame thrash
    // - >= 16 GB: Keep responsive while preventing expensive detail recompute every sample

    if (totalPhysicalBytes < (2ULL * 1024 * 1024 * 1024))
    {
        // < 2 GB: Cache longer to reduce CPU load on memory-constrained systems
        return {.light = std::chrono::milliseconds(4000), .heavy = std::chrono::milliseconds(15000)};
    }
    if (totalPhysicalBytes < (4ULL * 1024 * 1024 * 1024))
    {
        // 2-4 GB: Conservative but not aggressive
        return {.light = std::chrono::milliseconds(3000), .heavy = std::chrono::milliseconds(10000)};
    }
    if (totalPhysicalBytes < (8ULL * 1024 * 1024 * 1024))
    {
        // 4-8 GB: Balanced refresh
        return {.light = std::chrono::milliseconds(2000), .heavy = std::chrono::milliseconds(8000)};
    }
    if (totalPhysicalBytes < (16ULL * 1024 * 1024 * 1024))
    {
        // 8-16 GB: Moderate refresh
        return {.light = std::chrono::milliseconds(1500), .heavy = std::chrono::milliseconds(6000)};
    }
    // >= 16 GB: Responsive, but avoid 1Hz+ heavy metadata refresh churn
    return {.light = std::chrono::milliseconds(1000), .heavy = std::chrono::milliseconds(4000)};
}

/// Scheduler states of a thread (KTHREAD_STATE, SYSTEM_THREAD_INFORMATION::ThreadState) that
/// deriveProcessState() tells apart, spelled out so this header stays free of <windows.h>.
inline constexpr std::uint32_t THREAD_STATE_READY = 1;
inline constexpr std::uint32_t THREAD_STATE_RUNNING = 2;
inline constexpr std::uint32_t THREAD_STATE_STANDBY = 3;
inline constexpr std::uint32_t THREAD_STATE_WAITING = 5;
inline constexpr std::uint32_t THREAD_STATE_TRANSITION = 6; // Ready, but its kernel stack is paged out
inline constexpr std::uint32_t THREAD_STATE_DEFERRED_READY = 7;
inline constexpr std::uint32_t THREAD_STATE_GATE_WAIT = 8;
inline constexpr std::uint32_t THREAD_STATE_WAITING_FOR_PROCESS_IN_SWAP = 9;
/// Wait reasons (KWAIT_REASON, SYSTEM_THREAD_INFORMATION::WaitReason) of a suspended thread.
inline constexpr std::uint32_t WAIT_REASON_SUSPENDED = 5;
inline constexpr std::uint32_t WAIT_REASON_WR_SUSPENDED = 12;

/// Tally of one process's threads from the SystemProcessInformation snapshot, for a Linux-style
/// state letter (#1156). Windows has no process state of its own; every live process used to read
/// "R" (from GetExitCodeProcess), and one the probe couldn't open "?".
struct ProcessThreadTally
{
    std::size_t runnable = 0;  // Running, or ready to run (Ready, Standby, DeferredReady, Transition)
    std::size_t waiting = 0;   // Waiting for anything
    std::size_t suspended = 0; // Of those, waiting because the thread is suspended

    constexpr void add(std::uint32_t threadState, std::uint32_t waitReason) noexcept
    {
        switch (threadState)
        {
        case THREAD_STATE_READY:
        case THREAD_STATE_RUNNING:
        case THREAD_STATE_STANDBY:
        case THREAD_STATE_TRANSITION:
        case THREAD_STATE_DEFERRED_READY:
            ++runnable;
            break;
        case THREAD_STATE_WAITING:
            ++waiting;
            if (waitReason == WAIT_REASON_SUSPENDED || waitReason == WAIT_REASON_WR_SUSPENDED)
            {
                ++suspended;
            }
            break;
        case THREAD_STATE_GATE_WAIT:
        case THREAD_STATE_WAITING_FOR_PROCESS_IN_SWAP:
            ++waiting;
            break;
        default:
            break; // Initialized or Terminated: neither runs nor waits
        }
    }
};

/// The state letter for a process, with the meanings Linux gives them (#1156):
///  - 'I' Idle: the System Idle Process (PID 0), whose threads run whenever a CPU has nothing to do.
///  - 'R' Running: at least one thread is running or ready to run.
///  - 'T' Stopped: every thread is suspended -- a suspended or frozen (UWP) process, or one stopped
///    in a debugger.
///  - 'S' Sleeping: every thread is waiting, not all of them suspended.
///  - '?' Unknown: no thread to judge by. Minimal processes (Secure System, Registry, Memory
///    Compression) show no threads. So does a process that has exited but is kept by an open
///    handle, if it is listed: Windows has no reaping, so 'Z' (zombie) is never reported.
[[nodiscard]] constexpr char deriveProcessState(const ProcessThreadTally& threads, bool isIdleProcess) noexcept
{
    if (isIdleProcess)
    {
        return 'I';
    }
    if (threads.runnable > 0)
    {
        return 'R';
    }
    if (threads.waiting == 0)
    {
        return '?';
    }
    return threads.suspended == threads.waiting ? 'T' : 'S';
}

/// Which TTL-cached details getProcessDetails() refreshes for one process this sample (#1156).
struct DetailRefreshPlan
{
    bool light = false;    // Status, GDI objects
    bool heavy = false;    // Owner, command line, publisher, affinity, classification
    bool priority = false; // Priority class (GetPriorityClass)

    [[nodiscard]] constexpr bool any() const noexcept
    {
        return light || heavy || priority;
    }
};

/// Decide what to refresh for one process (#1156).
///  - Everything for a process seen for the first time.
///  - Light and heavy details when their TTLs are due.
///  - The priority class with the heavy details, and also as soon as the process's base priority in
///    the snapshot changes. Setting a priority class sets the base priority, so a change (by our
///    own Set Priority action or anyone else's) shows on the next sample rather than up to a heavy
///    TTL (4-15 s) later, without reading every process's class every sample. The class stays the
///    source of the value: a few system processes (csrss.exe, smss.exe) run at a base priority
///    their Normal class doesn't give.
[[nodiscard]] constexpr DetailRefreshPlan planDetailRefresh(bool firstSeen, bool lightDue, bool heavyDue, bool basePriorityChanged) noexcept
{
    const bool heavy = firstSeen || heavyDue;
    return DetailRefreshPlan{.light = firstSeen || lightDue, .heavy = heavy, .priority = heavy || basePriorityChanged};
}

/// Mark which of one process's readings the Windows probe took (#1285, the Windows half of #1110).
///  - Handle count and I/O bytes come from the SystemProcessInformation snapshot, which the kernel
///    fills for every process without an access check -- protected and other users' processes
///    included -- so they are always real readings, never a placeholder 0.
///  - Network bytes come from TCP EStats, which needs an elevated process (#1161). Each connection
///    is attributed to its process by the owner-PID TCP table, which needs no access to the process
///    either, so per-process network counters are read for every process or for none. With them off
///    (not elevated, EStats unsupported, or disabled after a real sample proved it unusable) no
///    process's network bytes were read: they are unavailable, not 0 -- as Linux reports I/O when
///    /proc/[pid]/io can't be read at all.
/// @param perProcessNetworkCounters Whether TCP EStats per-process counters are on
///                                  (ProcessCapabilities::hasNetworkCounters).
inline void markWindowsReadAvailability(ProcessCounters& counters, bool perProcessNetworkCounters) noexcept
{
    counters.handleCountAvailable = true;
    counters.ioCountersAvailable = true;
    counters.networkCountersAvailable = perProcessNetworkCounters;
}

/// MIB_TCP_STATE_ESTAB: the only TCP state whose EStats byte counters are worth reading. LISTEN,
/// TIME_WAIT, CLOSE_WAIT, etc. carry no meaningful per-connection byte counts.
inline constexpr std::uint32_t TCP_STATE_ESTABLISHED = 5;

/// Per-connection sanity cap: an EStats byte counter above 1 TB for a single connection is
/// garbage/uninitialized data, not real traffic.
inline constexpr std::uint64_t MAX_SANE_ESTATS_CONNECTION_BYTES = 1'000'000'000'000ULL; // 1 TB

/// What classifyEStatsRow() made of one TCP table row.
enum class EStatsRowOutcome : std::uint8_t
{
    SkippedState, // Not ESTABLISHED: no EStats calls are made for it
    ReadFailed,   // GetPerTcp[6]ConnectionEStats returned an error
    Garbage,      // A counter exceeded MAX_SANE_ESTATS_CONNECTION_BYTES
    Accumulated,  // A sane read: the connection's byte counts are reported
};

/// Per-row decision shared by the IPv4 and IPv6 EStats loops (#1100), extracted so both address
/// families decide identically and the decision is unit-testable without a live TCP table.
/// The caller still checks the state first so it can skip the Set/Get EStats calls for
/// non-ESTABLISHED rows; the check is repeated here so the function is self-contained.
/// @param readStatus Return value of GetPerTcp[6]ConnectionEStats (0 == NO_ERROR); ignored for
///                   non-ESTABLISHED rows.
[[nodiscard]] constexpr EStatsRowOutcome
classifyEStatsRow(std::uint32_t state, std::uint32_t readStatus, std::uint64_t bytesOut, std::uint64_t bytesIn) noexcept
{
    if (state != TCP_STATE_ESTABLISHED)
    {
        return EStatsRowOutcome::SkippedState;
    }
    if (readStatus != 0)
    {
        return EStatsRowOutcome::ReadFailed;
    }
    if (bytesOut > MAX_SANE_ESTATS_CONNECTION_BYTES || bytesIn > MAX_SANE_ESTATS_CONNECTION_BYTES)
    {
        return EStatsRowOutcome::Garbage;
    }
    return EStatsRowOutcome::Accumulated;
}

/// Win32 error codes the EStats tallies distinguish, spelled out so this header stays free of
/// <windows.h>; WindowsProcessProbe.cpp static_asserts they match the SDK values.
inline constexpr std::uint32_t ESTATS_NO_ERROR = 0;            // NO_ERROR
inline constexpr std::uint32_t ESTATS_ERROR_ACCESS_DENIED = 5; // ERROR_ACCESS_DENIED
inline constexpr std::uint32_t ESTATS_ERROR_NOT_FOUND = 1168;  // ERROR_NOT_FOUND

/// Per-sample tallies from one or more EStats table walks (IPv4 + IPv6), used for the periodic
/// debug line and for classifyEStatsProbe() (#1161).
struct EStatsSampleCounts
{
    std::size_t total = 0;           // Rows in the TCP table(s)
    std::size_t established = 0;     // ESTABLISHED rows (the only ones EStats is attempted on)
    std::size_t enabled = 0;         // SetPerTcp[6]ConnectionEStats succeeded
    std::size_t alreadyEnabled = 0;  // Not re-enabled: enabled on an earlier sample (#1418)
    std::size_t readOk = 0;          // GetPerTcp[6]ConnectionEStats succeeded (sane or garbage)
    std::size_t saneReads = 0;       // Read OK and within the 1 TB sanity cap (accumulated)
    std::size_t readNotFound = 0;    // Read returned ERROR_NOT_FOUND: connection closed mid-walk
    std::size_t readFailedOther = 0; // Read failed with anything but NOT_FOUND / ACCESS_DENIED
    std::size_t accessDenied = 0;    // Either call returned ERROR_ACCESS_DENIED
    std::size_t hasData = 0;         // Read OK, sane, and at least one non-zero counter
    std::size_t garbage = 0;         // Read OK but rejected by the 1 TB sanity cap

    EStatsSampleCounts& operator+=(const EStatsSampleCounts& other) noexcept
    {
        total += other.total;
        established += other.established;
        enabled += other.enabled;
        alreadyEnabled += other.alreadyEnabled;
        readOk += other.readOk;
        saneReads += other.saneReads;
        readNotFound += other.readNotFound;
        readFailedOther += other.readFailedOther;
        accessDenied += other.accessDenied;
        hasData += other.hasData;
        garbage += other.garbage;
        return *this;
    }
};

/// Record one ESTABLISHED-or-not table row: classify it via classifyEStatsRow() and tally the
/// Set/Get results into @p counts (#1161). Shared by the IPv4 and IPv6 walks so the tallies
/// classifyEStatsProbe() judges are unit-testable from fabricated error codes.
/// @param enableStatus Return value of SetPerTcp[6]ConnectionEStats, or std::nullopt if no enable
///                     was attempted.
/// @param readStatus   Return value of GetPerTcp[6]ConnectionEStats.
inline EStatsRowOutcome recordEStatsRow(EStatsSampleCounts& counts,
                                        std::uint32_t state,
                                        std::optional<std::uint32_t> enableStatus,
                                        std::uint32_t readStatus,
                                        std::uint64_t bytesOut,
                                        std::uint64_t bytesIn)
{
    const EStatsRowOutcome outcome = classifyEStatsRow(state, readStatus, bytesOut, bytesIn);
    if (outcome == EStatsRowOutcome::SkippedState)
    {
        return outcome;
    }

    ++counts.established;
    if (enableStatus == ESTATS_NO_ERROR)
    {
        ++counts.enabled;
    }
    if (enableStatus == ESTATS_ERROR_ACCESS_DENIED || readStatus == ESTATS_ERROR_ACCESS_DENIED)
    {
        ++counts.accessDenied;
    }

    switch (outcome)
    {
    case EStatsRowOutcome::Accumulated:
        ++counts.readOk;
        ++counts.saneReads;
        if (bytesOut > 0 || bytesIn > 0)
        {
            ++counts.hasData;
        }
        break;
    case EStatsRowOutcome::Garbage:
        ++counts.readOk;
        ++counts.garbage; // > 1 TB: garbage/uninitialized data, connection skipped
        break;
    case EStatsRowOutcome::ReadFailed:
        if (readStatus == ESTATS_ERROR_NOT_FOUND)
        {
            ++counts.readNotFound;
        }
        else if (readStatus != ESTATS_ERROR_ACCESS_DENIED)
        {
            ++counts.readFailedOther;
        }
        break;
    case EStatsRowOutcome::SkippedState:
        break;
    }
    return outcome;
}

/// Remembers which ESTABLISHED connections already have EStats collection enabled, so
/// SetPerTcp[6]ConnectionEStats is called once per connection instead of on every sample (#1418).
/// Enabling is sticky per connection; re-enabling every connection every sample was N redundant
/// kernel calls per sample on top of the N reads.
///
/// A connection counts as enabled only after evidence that it is: an enable that returned
/// ESTATS_NO_ERROR (this sample or an earlier one) followed by a sane read with data. Anything else
/// forgets it, so the next sample enables it again:
///  - A failed enable (access denied, not found, ...): never remembered, so every sample still
///    hands recordEStatsRow() a real enable status for it. The #1161 / #1285 / #1358 denial
///    detection keeps seeing every enable failure it saw before.
///  - A failed or garbage read, or a sane read with both counters 0: collection may be off (a
///    reused 4-tuple, or another tool turned it off), and re-enabling an enabled connection is
///    harmless -- it is what every sample used to do.
///
/// A known connection's read is recorded with enableStatus std::nullopt (no enable attempted), as
/// when the Set function is unavailable. recordEStatsRow() then tallies no `enabled`, which
/// classifyEStatsProbe() never consults: its verdict rests on accessDenied, established,
/// saneReads and readFailedOther, which come from the read alone. A sample of only known
/// connections that read sanely is Available, exactly as when each was re-enabled first.
///
/// Key reuse: estatsConnectionKey() hashes the 4-tuple, so a connection that closes and is
/// replaced by a new one on the same 4-tuple between two samples keeps a remembered key. That is
/// benign: the new connection was never enabled, so its read fails or reads 0 bytes, which forgets
/// the key, and the next sample enables it. (A connection missing from a complete walk is pruned
/// by endSample(), so a 4-tuple reused after a sample without it starts over anyway.)
///
/// Not thread-safe: the owner serializes access (WindowsProcessProbe holds it under a mutex).
class EStatsEnableTracker
{
  public:
    /// True when @p key has no remembered enable: call SetPerTcp[6]ConnectionEStats for it.
    [[nodiscard]] bool needsEnable(std::uint64_t key) const
    {
        return !m_LastSeen.contains(key);
    }

    /// Record one row's result for this sample.
    /// @param enableStatus Return value of SetPerTcp[6]ConnectionEStats, or std::nullopt when no
    ///                     enable was attempted (needsEnable() was false, or no Set function).
    /// @param outcome      recordEStatsRow()'s outcome for the row; SkippedState rows are ignored.
    /// @param hasData      The read reported at least one non-zero byte counter.
    void record(std::uint64_t key, std::optional<std::uint32_t> enableStatus, EStatsRowOutcome outcome, bool hasData)
    {
        if (outcome == EStatsRowOutcome::SkippedState)
        {
            return;
        }
        const bool enabled = enableStatus.has_value() ? *enableStatus == ESTATS_NO_ERROR : m_LastSeen.contains(key);
        if (enabled && outcome == EStatsRowOutcome::Accumulated && hasData)
        {
            m_LastSeen.insert_or_assign(key, m_Sample);
        }
        else
        {
            m_LastSeen.erase(key);
        }
    }

    /// Close one sample (both address families). @p complete: every TCP table was read, so a
    /// remembered connection not recorded this sample has closed and is forgotten. After an
    /// incomplete walk nothing is pruned: the unread table's connections may still be open.
    void endSample(bool complete)
    {
        if (complete)
        {
            std::erase_if(m_LastSeen, [sample = m_Sample](const auto& entry) { return entry.second != sample; });
        }
        ++m_Sample;
    }

    /// Connections currently remembered as enabled.
    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_LastSeen.size();
    }

  private:
    std::unordered_map<std::uint64_t, std::uint64_t> m_LastSeen; // Key -> last sample it was recorded enabled
    std::uint64_t m_Sample = 0;
};

/// Verdict of classifyEStatsProbe() on whether EStats per-process network counters really work.
enum class EStatsProbeResult : std::uint8_t
{
    Available,    // At least one established connection was read sanely, and nothing was denied
    Unavailable,  // Access was denied, or reads failed for a non-transient reason
    Undetermined, // Nothing conclusive this sample (idle, or only transient failures): try again
};

/// Consecutive inconclusive samples (established connections, but only NOT_FOUND reads and/or
/// garbage counters) after which classifyEStatsProbe() gives up and reports Unavailable (#1161).
/// One such sample is an ordinary race - every snapshotted connection closed before its EStats
/// read - but the same outcome on this many samples in a row means the reads never work.
inline constexpr std::size_t MAX_INCONCLUSIVE_ESTATS_SAMPLES = 3;

/// Decide from a real sample whether EStats is usable (#1161). The constructor's probe on a
/// zeroed dummy row can return ERROR_NOT_FOUND before (or instead of) the OS access check, so it
/// cannot prove availability; only real ESTABLISHED rows can.
///  - Any ERROR_ACCESS_DENIED proves the counters unusable: Unavailable at once.
///  - At least one sane (within the 1 TB cap) successful read proves them real: Available.
///  - Reads failing with anything but ERROR_NOT_FOUND, and none sane, prove them broken:
///    Unavailable at once (a non-transient error does not fix itself next sample).
///  - Only ERROR_NOT_FOUND failures and/or garbage counters are inconclusive: a connection can
///    close between the table snapshot and its read. Undetermined, unless this is the
///    MAX_INCONCLUSIVE_ESTATS_SAMPLES-th such sample in a row, which is no longer plausibly a race.
/// @param priorInconclusiveSamples Inconclusive samples (as above) already seen in a row.
[[nodiscard]] inline EStatsProbeResult classifyEStatsProbe(const EStatsSampleCounts& counts,
                                                           std::size_t priorInconclusiveSamples = 0) noexcept
{
    if (counts.accessDenied > 0)
    {
        return EStatsProbeResult::Unavailable;
    }
    if (counts.established == 0)
    {
        return EStatsProbeResult::Undetermined;
    }
    if (counts.saneReads > 0)
    {
        return EStatsProbeResult::Available;
    }
    if (counts.readFailedOther > 0)
    {
        return EStatsProbeResult::Unavailable;
    }
    // Only NOT_FOUND reads and/or garbage counters this sample.
    if (priorInconclusiveSamples + 1 >= MAX_INCONCLUSIVE_ESTATS_SAMPLES)
    {
        return EStatsProbeResult::Unavailable;
    }
    return EStatsProbeResult::Undetermined;
}

/// Why per-process network counters are missing, as ProcessCapabilities reports it (#1358).
struct NetworkCounterDenial
{
    bool reducedPrivileges = false; ///< Not elevated: running as Administrator would restore them.
    bool blocked = false;           ///< Elevated, yet denied (policy or a driver): elevating would not help.
};

/// Split an EStats access denial by whether elevation could cure it (#1358). Non-elevated, TCP
/// EStats is denied for privilege (hasReducedPrivileges, the lock icon). Elevated, a denial -- at
/// the constructor's probe or on the first real sample (#1161) -- means EStats is blocked on this
/// system, which deserves its own explanation rather than none at all: hasReducedPrivileges was
/// false then, so the network columns went away with nothing saying why. A denial is never
/// reported while the counters are claimed.
[[nodiscard]] constexpr NetworkCounterDenial
classifyNetworkCounterDenial(bool isElevated, bool hasNetworkCounters, bool accessDenied) noexcept
{
    const bool denied = accessDenied && !hasNetworkCounters;
    return {.reducedPrivileges = denied && !isElevated, .blocked = denied && isElevated};
}

/// Address family of a TCP connection (#1256).
enum class TcpAddressFamily : std::uint8_t
{
    IPv4,
    IPv6,
};

/// The endpoints that identify one TCP connection for its lifetime, as the owner-PID TCP tables
/// report them (#1256). Kept free of <windows.h>; WindowsTcpRows.h fills it from the table rows.
struct TcpConnectionEndpoints
{
    TcpAddressFamily family = TcpAddressFamily::IPv4;
    std::array<std::uint8_t, 16> localAddr{}; // IPv4: the first 4 bytes, in network order
    std::uint32_t localScopeId = 0;           // IPv6 only
    std::uint32_t localPort = 0;              // As the table stores it; only the low 16 bits are used
    std::array<std::uint8_t, 16> remoteAddr{};
    std::uint32_t remoteScopeId = 0;
    std::uint32_t remotePort = 0;
};

/// A stable key for one TCP connection, for Domain::SocketTrafficAccumulator (#1256): a 64-bit
/// FNV-1a hash of the family, both addresses, scope ids and ports, in local-then-remote order, so
/// the same connection always has the same key and swapped endpoints (the other side of a loopback
/// connection) have different ones.
///  - Only the low 16 bits of each port are hashed: the owner-PID tables leave the upper 16 bits of
///    the port DWORDs undefined, which would change the key from one read to the next.
///  - The top bit is the family (clear for IPv4, set for IPv6), so an IPv4 and an IPv6 key never
///    collide.
///  - Never 0, which the accumulator reserves (it skips key 0).
[[nodiscard]] constexpr std::uint64_t estatsConnectionKey(const TcpConnectionEndpoints& endpoints) noexcept
{
    constexpr std::uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t FNV_PRIME = 0x100000001b3ULL;
    constexpr std::uint64_t FAMILY_BIT = 1ULL << 63U;
    constexpr std::uint32_t PORT_MASK = 0xFFFFU;

    const bool isIPv6 = endpoints.family == TcpAddressFamily::IPv6;
    const std::size_t addrBytes = isIPv6 ? 16 : 4;

    std::uint64_t hash = FNV_OFFSET_BASIS;
    const auto mixByte = [&hash](std::uint8_t byte)
    {
        hash ^= byte;
        hash *= FNV_PRIME;
    };
    const auto mixU32 = [&mixByte](std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            mixByte(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
        }
    };
    const auto mixEndpoint = [&](const std::array<std::uint8_t, 16>& addr, std::uint32_t scopeId, std::uint32_t port)
    {
        for (std::size_t i = 0; i < addrBytes; ++i)
        {
            mixByte(addr.at(i));
        }
        mixU32(isIPv6 ? scopeId : 0U);
        mixU32(port & PORT_MASK);
    };

    mixByte(isIPv6 ? 6U : 4U);
    mixEndpoint(endpoints.localAddr, endpoints.localScopeId, endpoints.localPort);
    mixEndpoint(endpoints.remoteAddr, endpoints.remoteScopeId, endpoints.remotePort);

    if (isIPv6)
    {
        return hash | FAMILY_BIT;
    }
    const std::uint64_t key = hash & ~FAMILY_BIT;
    return key == 0 ? 1 : key;
}

/// One ESTABLISHED connection's EStats read in one walk of the TCP tables (#1256).
struct EStatsConnectionRead
{
    std::uint64_t key = 0; // estatsConnectionKey()
    std::uint32_t pid = 0; // Owning PID from the table row
    EStatsRowOutcome outcome = EStatsRowOutcome::ReadFailed;
    std::uint64_t bytesReceived = 0; // DataBytesIn; meaningful only when outcome == Accumulated
    std::uint64_t bytesSent = 0;     // DataBytesOut; likewise
};

/// Turn one complete walk of the TCP tables into the per-connection samples readSocketTraffic()
/// reports (#1256). Stateless: Domain::SocketTrafficAccumulator keeps the per-connection baselines.
///
/// A connection whose EStats read failed or returned garbage this walk is still in the table, so
/// it is reported unreadable (SocketTrafficSample::readable false) rather than left out: the
/// accumulator would take a connection missing from one reading as closed, and the same connection
/// back in the next as new, crediting all its lifetime bytes to the process in one interval.
/// Reported unreadable, it keeps its baseline instead. Rows not in ESTABLISHED (SkippedState) are
/// left out.
///
/// Call only for a complete walk: if either table could not be read, report no reading.
/// @param reads  This walk's ESTABLISHED rows (SkippedState rows are ignored).
[[nodiscard]] inline std::vector<SocketTrafficSample> buildSocketTrafficSamples(std::span<const EStatsConnectionRead> reads)
{
    std::vector<SocketTrafficSample> samples;
    samples.reserve(reads.size());
    for (const auto& read : reads)
    {
        const auto pid = static_cast<std::int32_t>(read.pid);
        if (read.outcome == EStatsRowOutcome::Accumulated)
        {
            samples.push_back(
                SocketTrafficSample{.key = read.key, .pid = pid, .bytesReceived = read.bytesReceived, .bytesSent = read.bytesSent});
        }
        else if (read.outcome == EStatsRowOutcome::ReadFailed || read.outcome == EStatsRowOutcome::Garbage)
        {
            samples.push_back(SocketTrafficSample{.key = read.key, .pid = pid, .readable = false});
        }
    }
    return samples;
}

/// The reading readSocketTraffic() reports for one walk of the TCP tables (#1256).
/// @param complete      Both tables were read. If not, the walk is missing connections that are
///                      still open, and reported they would look closed and then, back in the next
///                      reading, new -- crediting their lifetime bytes. So no reading is reported
///                      (sampleTimeNs 0, which Domain doesn't fold).
/// @param sampleTimeNs  When the walk was taken (steady_clock ns); non-zero.
[[nodiscard]] inline SocketTrafficReading
makeSocketTrafficReading(std::span<const EStatsConnectionRead> reads, bool complete, std::uint64_t sampleTimeNs)
{
    if (!complete)
    {
        return {};
    }
    return SocketTrafficReading{.sockets = buildSocketTrafficSamples(reads), .sampleTimeNs = sampleTimeNs};
}

// ==========================================================================
// CPU affinity across processor groups (#1247)
// ==========================================================================

/// One processor group's layout: how many processors it has room for (GetMaximumProcessorCount,
/// fixed for the boot session, read once) and which of them are active (PROCESSOR_GROUP_INFO::
/// ActiveProcessorMask, which hot-add or offlining can change, so re-read on the heavy cadence).
struct ProcessorGroupLayout
{
    std::uint32_t maximumProcessors = 0;
    std::uint64_t activeMask = 0;
};

/// How readCpuAffinity() may read a process's affinity, from what processor-topology discovery found.
enum class AffinityTopology : std::uint8_t
{
    SingleGroup, // One group: GetProcessAffinityMask's mask is the whole affinity, bit N = CPU N
    MultiGroup,  // Several groups, each with a current active mask: the group-aware reads
    Unknown,     // Discovery failed: the affinity is reported unreadable rather than guessed
};

/// @param groupCount       GetMaximumProcessorGroupCount (0 if it failed).
/// @param activeMasksRead  Whether the groups' active masks were read (needed only for several).
/// A failed discovery is never taken for one group: on a multi-group machine that would map one
/// group's mask onto CPUs 0-63 -- the partial value #1247 fixes.
[[nodiscard]] constexpr AffinityTopology affinityTopology(std::size_t groupCount, bool activeMasksRead) noexcept
{
    if (groupCount == 1)
    {
        return AffinityTopology::SingleGroup;
    }
    if (groupCount == 0 || !activeMasksRead)
    {
        return AffinityTopology::Unknown;
    }
    return AffinityTopology::MultiGroup;
}

/// Whether this Windows build's threads have, by default, an affinity spanning every processor
/// group: Windows 11 and Windows Server 2022 (build 20348) on. Such a thread's
/// GetThreadGroupAffinity reports only its primary group. An unknown build (0) is assumed to.
[[nodiscard]] constexpr bool threadsMaySpanGroups(std::uint32_t buildNumber) noexcept
{
    constexpr std::uint32_t SERVER_2022_BUILD = 20348; // Windows 11 is 22000
    return buildNumber == 0 || buildNumber >= SERVER_2022_BUILD;
}

/// An affinity within one processor group: bit N is processor N of `group` (a GROUP_AFFINITY).
struct GroupAffinityMask
{
    std::uint16_t group = 0;
    std::uint64_t mask = 0;
};

/// Maps per-group affinity masks to one CpuAffinity numbered as the per-core CPU figures are:
/// processor N of group G is CPU (sum of groups 0..G-1's maximum processor counts) + N, the coreId
/// WindowsSystemProbe gives it (processorGroupFirstCoreIds(), #1107). Maximum counts, not active
/// ones, so a hot-added processor never renumbers a later group's CPUs.
///
/// Masks for the same group are combined. A group the layout doesn't have, and bits at or above a
/// group's maximum count, are dropped: they name no processor. With a single group of 64 or fewer
/// processors this is CpuAffinity::fromMask(mask) of group 0's mask.
[[nodiscard]] inline CpuAffinity cpuAffinityFromGroupMasks(std::span<const GroupAffinityMask> masks,
                                                           std::span<const ProcessorGroupLayout> groups)
{
    CpuAffinity affinity;
    std::size_t firstCpu = 0;
    for (std::size_t group = 0; group < groups.size(); ++group)
    {
        const std::uint32_t size = groups[group].maximumProcessors;
        const std::uint64_t valid = (size >= CpuAffinity::BITS_PER_WORD) ? ~std::uint64_t{0} : ((std::uint64_t{1} << size) - 1U);
        std::uint64_t bits = 0;
        for (const GroupAffinityMask& entry : masks)
        {
            if (entry.group == group)
            {
                bits |= entry.mask;
            }
        }
        bits &= valid;
        while (bits != 0)
        {
            // One setRange() per run of consecutive processors.
            const auto low = static_cast<std::size_t>(std::countr_zero(bits));
            const auto run = static_cast<std::size_t>(std::countr_one(bits >> low));
            affinity.setRange(firstCpu + low, firstCpu + low + run - 1);
            bits = (low + run >= CpuAffinity::BITS_PER_WORD) ? 0 : (bits & ~((std::uint64_t{1} << (low + run)) - 1U));
        }
        firstCpu += size;
    }
    return affinity;
}

/// A process's per-group masks from the two process-level reads, when they are enough; nullopt
/// when its threads must be asked (groupMasksFromThreads()).
/// @param processGroups GetProcessGroupAffinity: the groups the process's threads are assigned to.
/// @param processMask   GetProcessAffinityMask's process mask. It describes one group only: the
///                      process's single group, or (Windows 11 / Server 2022+, where a process spans
///                      every group by default) its primary group -- which no documented call names
///                      for another process. It is 0 when the process has threads explicitly
///                      assigned to several groups.
/// One group: that group's mask is processMask -- except, when threadsMaySpan, a mask equal to the
/// group's every active processor. A process that spans every group by default (the Windows 11 /
/// Server 2022+ default) reads exactly like that, as does one deliberately confined to its whole
/// primary group, and nothing tells the two apart. The default is by far the common case, so it is
/// read as the span over every group's active processors (#1247). Several groups, with a non-zero
/// processMask equal to every one of those groups' active masks: the default span over all of
/// them, unrestricted. Anything else (processMask 0, or several groups with a restricted mask
/// somewhere) needs the threads.
/// @param threadsMaySpan threadsMaySpanGroups() for this build. Before it, a process runs in one
///                       group, so its one group's mask is exact.
[[nodiscard]] inline std::optional<std::vector<GroupAffinityMask>> groupMasksFromProcess(std::span<const std::uint16_t> processGroups,
                                                                                         std::uint64_t processMask,
                                                                                         std::span<const ProcessorGroupLayout> groups,
                                                                                         bool threadsMaySpan)
{
    if (processMask == 0)
    {
        return std::nullopt;
    }
    if (processGroups.size() == 1)
    {
        const std::uint16_t group = processGroups.front();
        if (threadsMaySpan && groups.size() > 1 && group < groups.size() && groups[group].activeMask == processMask)
        {
            std::vector<GroupAffinityMask> span;
            span.reserve(groups.size());
            for (std::size_t g = 0; g < groups.size(); ++g)
            {
                span.push_back({.group = static_cast<std::uint16_t>(g), .mask = groups[g].activeMask});
            }
            return span;
        }
        return std::vector<GroupAffinityMask>{{.group = group, .mask = processMask}};
    }
    std::vector<GroupAffinityMask> masks;
    masks.reserve(processGroups.size());
    for (const std::uint16_t group : processGroups)
    {
        if (group >= groups.size() || groups[group].activeMask != processMask)
        {
            return std::nullopt;
        }
        masks.push_back({.group = group, .mask = processMask});
    }
    return masks;
}

/// What a process's threads' GetThreadGroupAffinity reads found.
struct ThreadGroupAffinityReads
{
    std::vector<GroupAffinityMask> masks; // One per thread read
    bool complete = false;                // Every thread in the snapshot was opened and read
};

/// A process's per-group masks from its threads' GetThreadGroupAffinity reads, for when
/// groupMasksFromProcess() can't tell: the union of the threads' masks. Empty -- unreadable --
/// whenever that union might not be the affinity, rather than a guess:
///  - Not every thread was read (one exited, or denied access): the missing one may have been the
///    only thread in a group, or the only one allowed some processor.
///  - A listed group no thread reports: on Windows 11 / Server 2022+ a thread that spans every group
///    by default reports only its primary group, so the absent group may be spanned in full -- or,
///    the reads having raced a thread's move, restricted. Nothing says which.
///  - threadsMaySpan, and a thread reports its group's every active processor: that thread may be
///    bound to that group or may span every group by default; GetThreadGroupAffinity reads the same
///    for both. Only when the union already covers every active processor do the two agree.
/// @param threadsMaySpan threadsMaySpanGroups() for this build. Before it, each thread runs in one
///                       group, so a full mask is that group alone.
[[nodiscard]] inline std::vector<GroupAffinityMask> groupMasksFromThreads(std::span<const std::uint16_t> processGroups,
                                                                          const ThreadGroupAffinityReads& threads,
                                                                          std::span<const ProcessorGroupLayout> groups,
                                                                          bool threadsMaySpan)
{
    if (!threads.complete || threads.masks.empty())
    {
        return {};
    }
    const auto reportedMask = [&threads](std::size_t group)
    {
        std::uint64_t mask = 0;
        for (const GroupAffinityMask& entry : threads.masks)
        {
            if (entry.group == group)
            {
                mask |= entry.mask;
            }
        }
        return mask;
    };
    for (const std::uint16_t group : processGroups)
    {
        if (reportedMask(group) == 0)
        {
            return {};
        }
    }
    if (threadsMaySpan)
    {
        const bool someThreadFull = std::ranges::any_of(
            threads.masks,
            [groups](const GroupAffinityMask& entry)
            { return entry.group < groups.size() && groups[entry.group].activeMask != 0 && entry.mask == groups[entry.group].activeMask; });
        if (someThreadFull)
        {
            for (std::size_t group = 0; group < groups.size(); ++group)
            {
                const std::uint64_t active = groups[group].activeMask;
                if ((reportedMask(group) & active) != active)
                {
                    return {}; // Bound to its group, or spanning them all: can't tell
                }
            }
        }
    }
    return threads.masks;
}

} // namespace Platform
