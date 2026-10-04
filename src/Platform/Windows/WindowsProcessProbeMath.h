#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>

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

/// MIB_TCP_STATE_ESTAB: the only TCP state whose EStats byte counters are worth reading. LISTEN,
/// TIME_WAIT, CLOSE_WAIT, etc. carry no meaningful per-connection byte counts.
inline constexpr std::uint32_t TCP_STATE_ESTABLISHED = 5;

/// Per-connection sanity cap: an EStats byte counter above 1 TB for a single connection is
/// garbage/uninitialized data, not real traffic.
inline constexpr std::uint64_t MAX_SANE_ESTATS_CONNECTION_BYTES = 1'000'000'000'000ULL; // 1 TB

/// Cumulative (sent, received) EStats byte counts keyed by owning PID.
using PerPidNetworkBytes = std::unordered_map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>>;

/// What accumulateEStatsRow() did with one TCP table row.
enum class EStatsRowOutcome : std::uint8_t
{
    SkippedState, // Not ESTABLISHED: no EStats calls are made for it
    ReadFailed,   // GetPerTcp[6]ConnectionEStats returned an error
    Garbage,      // A counter exceeded MAX_SANE_ESTATS_CONNECTION_BYTES
    Accumulated,  // Byte counts were added to the owning PID
};

/// Per-row decision shared by the IPv4 and IPv6 EStats loops (#1100), extracted so both address
/// families aggregate identically and the decision is unit-testable without a live TCP table.
/// The caller still checks the state first so it can skip the Set/Get EStats calls for
/// non-ESTABLISHED rows; the check is repeated here so the function is self-contained.
/// @param readStatus Return value of GetPerTcp[6]ConnectionEStats (0 == NO_ERROR); ignored for
///                   non-ESTABLISHED rows.
inline EStatsRowOutcome accumulateEStatsRow(PerPidNetworkBytes& perPid,
                                            std::uint32_t pid,
                                            std::uint32_t state,
                                            std::uint32_t readStatus,
                                            std::uint64_t bytesOut,
                                            std::uint64_t bytesIn)
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

    auto& agg = perPid[pid];
    agg.first += bytesOut;
    agg.second += bytesIn;
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

/// Record one ESTABLISHED-or-not table row: accumulate its bytes via accumulateEStatsRow() and
/// tally the Set/Get results into @p counts (#1161). Shared by the IPv4 and IPv6 walks so the
/// tallies classifyEStatsProbe() judges are unit-testable from fabricated error codes.
/// @param enableStatus Return value of SetPerTcp[6]ConnectionEStats, or std::nullopt if no enable
///                     was attempted.
/// @param readStatus   Return value of GetPerTcp[6]ConnectionEStats.
inline EStatsRowOutcome recordEStatsRow(EStatsSampleCounts& counts,
                                        PerPidNetworkBytes& perPid,
                                        std::uint32_t pid,
                                        std::uint32_t state,
                                        std::optional<std::uint32_t> enableStatus,
                                        std::uint32_t readStatus,
                                        std::uint64_t bytesOut,
                                        std::uint64_t bytesIn)
{
    const EStatsRowOutcome outcome = accumulateEStatsRow(perPid, pid, state, readStatus, bytesOut, bytesIn);
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

} // namespace Platform
