#pragma once

// The CPU's static facts for the Overview's CPU Details block (#809): topology, base clock, cache
// sizes and, on Windows, virtualization status. Every field is optional, so a value the platform or
// hardware cannot supply is shown as unavailable (or hidden) rather than as a fake 0. Read once,
// when the system probe is built: none of these change during a boot session.
//
// The topology and cache arithmetic below is pure (no OS headers), so it is unit-tested on every
// platform; the Linux and Windows probes only gather the raw records it works on.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Platform
{

/// CpuDetails::efficiencyClassByCoreId for a core id with no reading.
inline constexpr std::uint8_t UNKNOWN_EFFICIENCY_CLASS = 0xFF;

/// What the system probe knows about the CPU beyond its model name. Unknown fields are nullopt.
struct CpuDetails
{
    std::optional<std::size_t> sockets;           ///< Physical packages.
    std::optional<std::size_t> physicalCores;     ///< Cores, not counting SMT siblings.
    std::optional<std::size_t> logicalProcessors; ///< Hardware threads, the same set SystemSnapshot::coreCount counts.
    /// On a hybrid CPU (more than one core efficiency class), the cores of the most performant class
    /// and the rest. nullopt on a CPU with one kind of core, or where the platform cannot tell.
    std::optional<std::size_t> performanceCores;
    std::optional<std::size_t> efficiencyCores;
    /// On a hybrid CPU, each logical processor's efficiency class (higher is more performant),
    /// indexed by core id like SystemSnapshot::cpuPerCore; UNKNOWN_EFFICIENCY_CLASS where an id has
    /// no reading. Empty on a CPU with one kind of core, or where the platform cannot tell.
    std::vector<std::uint8_t> efficiencyClassByCoreId;
    std::optional<std::uint64_t> baseSpeedMHz; ///< Rated (base) clock.
    /// Total cache at each level across every instance (L1 counts data and instruction caches), as
    /// Task Manager reports it.
    std::optional<std::uint64_t> l1CacheBytes;
    std::optional<std::uint64_t> l2CacheBytes;
    std::optional<std::uint64_t> l3CacheBytes;

    // Windows only (SystemCapabilities::hasVirtualizationInfo)
    std::optional<bool> virtualizationFirmwareEnabled; ///< Hardware virtualization enabled in firmware.
    std::optional<bool> slatSupported;                 ///< Second-level address translation.
    std::optional<bool> hypervisorPresent;             ///< A hypervisor is running (CPUID).
    std::optional<bool> vbsRunning;                    ///< Virtualization-based security's secure kernel is running.
    std::optional<bool> hvciEnabled;                   ///< Memory integrity (hypervisor-enforced code integrity).

    bool operator==(const CpuDetails&) const = default;
};

namespace CpuTopology
{

/// One cache instance: a cache of `bytes` at `level` (1, 2, 3; anything else is ignored).
struct CacheInstance
{
    unsigned level = 0;
    std::uint64_t bytes = 0;
};

/// Per-level totals of `instances`. A level with no instance (or only zero-sized ones) stays nullopt.
inline void sumCacheInstances(std::span<const CacheInstance> instances, CpuDetails& details) noexcept
{
    std::uint64_t l1 = 0;
    std::uint64_t l2 = 0;
    std::uint64_t l3 = 0;
    for (const CacheInstance& cache : instances)
    {
        switch (cache.level)
        {
        case 1:
            l1 += cache.bytes;
            break;
        case 2:
            l2 += cache.bytes;
            break;
        case 3:
            l3 += cache.bytes;
            break;
        default:
            break;
        }
    }
    const auto known = [](std::uint64_t total)
    {
        return (total > 0) ? std::optional<std::uint64_t>{total} : std::nullopt;
    };
    details.l1CacheBytes = known(l1);
    details.l2CacheBytes = known(l2);
    details.l3CacheBytes = known(l3);
}

/// One physical core: its efficiency class (higher is more performant; Windows'
/// PROCESSOR_RELATIONSHIP::EfficiencyClass) and how many logical processors it runs.
struct CoreRecord
{
    std::uint8_t efficiencyClass = 0;
    std::uint32_t logicalProcessors = 0;
};

/// Cores, logical processors and, on a hybrid CPU, the performance/efficiency split, from the cores'
/// records. Nothing is set when there are no records. A CPU whose cores share one efficiency class
/// is not hybrid, and its split stays nullopt.
inline void summarizeCores(std::span<const CoreRecord> cores, CpuDetails& details) noexcept
{
    if (cores.empty())
    {
        return;
    }
    std::size_t logical = 0;
    std::uint8_t lowest = cores.front().efficiencyClass;
    std::uint8_t highest = lowest;
    for (const CoreRecord& core : cores)
    {
        logical += core.logicalProcessors;
        lowest = std::min(lowest, core.efficiencyClass);
        highest = std::max(highest, core.efficiencyClass);
    }
    details.physicalCores = cores.size();
    if (logical > 0)
    {
        details.logicalProcessors = logical;
    }
    if (highest != lowest)
    {
        const auto performance = static_cast<std::size_t>(
            std::ranges::count_if(cores, [highest](const CoreRecord& core) { return core.efficiencyClass == highest; }));
        details.performanceCores = performance;
        details.efficiencyCores = cores.size() - performance;
    }
}

/// Record logical processor `coreId`'s efficiency class, growing `classes` as needed (#809).
inline void setEfficiencyClass(std::vector<std::uint8_t>& classes, std::size_t coreId, std::uint8_t efficiencyClass)
{
    if (coreId >= classes.size())
    {
        classes.resize(coreId + 1, UNKNOWN_EFFICIENCY_CLASS);
    }
    classes[coreId] = efficiencyClass;
}

/// Empty `classes` unless its known entries hold more than one class: a CPU with one kind of core
/// has no per-processor distinction worth carrying.
inline void keepOnlyIfHybrid(std::vector<std::uint8_t>& classes) noexcept
{
    const auto known = [](std::uint8_t c)
    {
        return c != UNKNOWN_EFFICIENCY_CLASS;
    };
    const auto first = std::ranges::find_if(classes, known);
    if (first == classes.end() || std::ranges::all_of(classes, [&](std::uint8_t c) { return !known(c) || c == *first; }))
    {
        classes.clear();
    }
}

} // namespace CpuTopology

} // namespace Platform
