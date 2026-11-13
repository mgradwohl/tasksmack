#pragma once

// The NUMA nodes as the snapshot carries them (#3088): each node's processors' average use, from the
// per-core figures, and its used memory, from the raw nodes the system probe read. Pure, so SystemModel
// and the tests share it.

#include "Domain/SystemSnapshot.h"
#include "Platform/CpuAffinity.h"
#include "Platform/SystemTypes.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Domain
{

/// The average busy % of the processors in @p cpus, over those with a reading in @p perCore (indexed by
/// core id; NaN where a core has none this sample, SystemSnapshot::cpuPerCore). NaN when none has one: a
/// CPU-less node, or the first sample, which has no deltas yet.
[[nodiscard]] inline double numaNodeCpuPercent(const Platform::CpuAffinity& cpus, std::span<const CpuUsage> perCore) noexcept
{
    double sum = 0.0;
    std::size_t counted = 0;
    const auto words = cpus.words();
    for (std::size_t word = 0; word < words.size(); ++word)
    {
        std::uint64_t bits = words[word];
        while (bits != 0)
        {
            const std::size_t cpu = (word * Platform::CpuAffinity::BITS_PER_WORD) + static_cast<std::size_t>(std::countr_zero(bits));
            bits &= bits - 1U;
            if (cpu < perCore.size() && !std::isnan(perCore[cpu].totalPercent))
            {
                sum += perCore[cpu].totalPercent;
                ++counted;
            }
        }
    }
    return (counted == 0) ? std::numeric_limits<double>::quiet_NaN() : sum / static_cast<double>(counted);
}

/// The snapshot's NUMA nodes from the probe's @p nodes, in their order. @p totalsKnown is
/// SystemCapabilities::hasNumaNodeTotals: without it (Windows) a node's total and used memory are 0 and
/// its free memory is what Windows calls available. Used memory saturates at 0, so a free figure above
/// the total never wraps.
[[nodiscard]] inline std::vector<NumaNodeSnapshot>
numaNodeSnapshots(std::span<const Platform::NumaNode> nodes, std::span<const CpuUsage> perCore, bool totalsKnown)
{
    std::vector<NumaNodeSnapshot> snapshots;
    snapshots.reserve(nodes.size());
    for (const Platform::NumaNode& node : nodes)
    {
        NumaNodeSnapshot& snap = snapshots.emplace_back();
        snap.id = node.id;
        snap.cpus = node.cpus;
        snap.cpuCount = node.cpus.count();
        snap.cpuPercent = numaNodeCpuPercent(node.cpus, perCore);
        snap.memFreeBytes = node.memFreeBytes;
        if (totalsKnown)
        {
            snap.memTotalBytes = node.memTotalBytes;
            snap.memUsedBytes = (node.memTotalBytes > node.memFreeBytes) ? node.memTotalBytes - node.memFreeBytes : 0;
        }
    }
    return snapshots;
}

} // namespace Domain
