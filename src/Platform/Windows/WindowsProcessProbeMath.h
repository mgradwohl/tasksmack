#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
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

/// Per-sample tallies from one or more EStats table walks (IPv4 + IPv6), used for the periodic
/// debug line and for classifyEStatsProbe() (#1161).
struct EStatsSampleCounts
{
    std::size_t total = 0;        // Rows in the TCP table(s)
    std::size_t established = 0;  // ESTABLISHED rows (the only ones EStats is attempted on)
    std::size_t enabled = 0;      // SetPerTcp[6]ConnectionEStats succeeded
    std::size_t readOk = 0;       // GetPerTcp[6]ConnectionEStats succeeded
    std::size_t accessDenied = 0; // Either call returned ERROR_ACCESS_DENIED
    std::size_t hasData = 0;      // Read OK, sane, and at least one non-zero counter
    std::size_t garbage = 0;      // Read OK but rejected by the 1 TB sanity cap

    EStatsSampleCounts& operator+=(const EStatsSampleCounts& other) noexcept
    {
        total += other.total;
        established += other.established;
        enabled += other.enabled;
        readOk += other.readOk;
        accessDenied += other.accessDenied;
        hasData += other.hasData;
        garbage += other.garbage;
        return *this;
    }
};

/// Verdict of classifyEStatsProbe() on whether EStats per-process network counters really work.
enum class EStatsProbeResult : std::uint8_t
{
    Available,    // At least one established connection was read, and nothing was denied
    Unavailable,  // Access was denied, or every established connection's read failed
    Undetermined, // No established connections this sample: keep trying on the next one
};

/// Decide from a real sample whether EStats is usable (#1161). The constructor's probe on a
/// zeroed dummy row can return ERROR_NOT_FOUND before (or instead of) the OS access check, so it
/// cannot prove availability; only real ESTABLISHED rows can. Any ERROR_ACCESS_DENIED, or
/// established connections none of which could be read, means the per-process network column
/// would be fabricated zeros.
[[nodiscard]] inline EStatsProbeResult classifyEStatsProbe(const EStatsSampleCounts& counts) noexcept
{
    if (counts.accessDenied > 0)
    {
        return EStatsProbeResult::Unavailable;
    }
    if (counts.established == 0)
    {
        return EStatsProbeResult::Undetermined;
    }
    if (counts.readOk == 0)
    {
        return EStatsProbeResult::Unavailable;
    }
    return EStatsProbeResult::Available;
}

} // namespace Platform
