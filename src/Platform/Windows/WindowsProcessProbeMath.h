#pragma once

#include "Platform/ProcessTypes.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

} // namespace Platform
