#pragma once

#include <chrono>
#include <cstdint>

namespace Platform
{

/// TTLs for the light/heavy detail-refresh caches, tuned by calculateDetailTTLsFromTotalRAMBytes().
struct DetailCacheTTLs
{
    std::chrono::milliseconds light;
    std::chrono::milliseconds heavy;
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

} // namespace Platform
