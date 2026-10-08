#pragma once

// The CPU facts for the Overview's CPU Details block (#809): topology, base clock, cache sizes and,
// on Windows, virtualization status. Every field is optional, so a value the platform or hardware
// cannot supply is shown as unavailable (or hidden) rather than as a fake 0. They are cached facts:
// each system probe reads them when it is built and again only when the set of active processors it
// samples changes (hotplug, hot-add, a CPU taken offline), committing a re-read only when it
// describes the processors of that sample -- see cpuDetailsNeedRead() and commitIfConsistent().
//
// The topology, cache and refresh logic below is pure (no OS headers), so it is unit-tested on every
// platform; the Linux and Windows probes only gather the raw records it works on.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
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

/// A probe's cached CPU details (#809): what it publishes, the logical processor ids they describe,
/// and any re-read that has not yet matched its sample.
struct CachedCpuDetails
{
    CpuDetails details;
    std::vector<std::size_t> ids;        ///< The processor ids `details` describe, ascending; empty = not known
    std::vector<std::size_t> pendingIds; ///< The sampled set the last inconsistent re-reads were for
    unsigned pendingAttempts = 0;        ///< How many re-reads in a row failed to match `pendingIds`
};

/// Inconsistent re-reads tried for one sampled set before giving up until the set changes again: a
/// re-read races the per-core read it is checked against (a CPU going on- or offline between the
/// two), so a mismatch is retried on the next samples, but a source that never agrees with the
/// per-core read (no processor numbers in /proc/cpuinfo, say) is not re-read every sample forever.
inline constexpr unsigned MAX_INCONSISTENT_CPU_DETAIL_READS = 3;

/// Whether a probe should re-read its CPU details for this sample (#809): `sampled` are the ids of
/// the processors the per-core read just sampled (ascending). Any difference from the ids the
/// details describe -- a processor brought online, taken offline or hot-added, even one swapped for
/// another at the same count -- asks for a re-read, unless that set has already failed
/// MAX_INCONSISTENT_CPU_DETAIL_READS re-reads. An empty sample (a failed per-core read) never does.
template<std::ranges::input_range SampledIds>
[[nodiscard]] bool cpuDetailsNeedRead(const CachedCpuDetails& cache, const SampledIds& sampled)
{
    if (std::ranges::empty(sampled) || std::ranges::equal(cache.ids, sampled))
    {
        return false;
    }
    return !(std::ranges::equal(cache.pendingIds, sampled) && cache.pendingAttempts >= MAX_INCONSISTENT_CPU_DETAIL_READS);
}

/// Commit freshly read details and the ids they describe, together, only when those ids equal this
/// sample's (#809): then the details, the per-core counters and the core count published with them
/// describe the same processors. On a mismatch -- the OS's processor set changed between the two
/// reads -- the cache keeps its previous details and ids, and the attempt is counted for
/// cpuDetailsNeedRead(), so the next sample retries. An empty sample commits nothing. Returns
/// whether it committed.
template<std::ranges::input_range SampledIds>
bool commitIfConsistent(const SampledIds& sampled, std::vector<std::size_t> describedIds, CpuDetails fresh, CachedCpuDetails& cache)
{
    if (std::ranges::empty(sampled))
    {
        return false;
    }
    if (std::ranges::equal(describedIds, sampled))
    {
        cache.details = std::move(fresh);
        cache.ids = std::move(describedIds);
        cache.pendingIds.clear();
        cache.pendingAttempts = 0;
        return true;
    }
    if (std::ranges::equal(cache.pendingIds, sampled))
    {
        ++cache.pendingAttempts;
    }
    else
    {
        cache.pendingIds.clear();
        for (const std::size_t id : sampled)
        {
            cache.pendingIds.push_back(id);
        }
        cache.pendingAttempts = 1;
    }
    return false;
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
