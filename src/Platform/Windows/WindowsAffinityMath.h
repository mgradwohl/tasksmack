#pragma once

// Setting a process's CPU affinity on Windows (#1881): the pure steps between the CpuAffinity the
// dialog hands on and the Win32 calls that set it, and the layout the dialog groups its boxes by.
// No Windows header, so the tests run on every platform (#1133).
//
// CPUs are numbered as everywhere else in TaskSmack (#1107, #1247): processor N of group G is CPU
// (sum of groups 0..G-1's maximum processor counts) + N.

#include "Platform/CpuAffinity.h"
#include "Platform/CpuDetails.h"
#include "Platform/IProcessActions.h"
#include "WindowsProcessProbeMath.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <vector>

namespace Platform
{

/// One physical core, from GetLogicalProcessorInformationEx(RelationProcessorCore): its processor
/// group, its logical processors in that group, and its efficiency class (higher is more performant).
struct ProcessorCoreRecord
{
    std::uint16_t group = 0;
    std::uint64_t mask = 0;
    std::uint8_t efficiencyClass = 0;
};

/// One CPU set, from GetSystemCpuSetInformation: the id SetProcessDefaultCpuSets() takes, and the
/// logical processor it stands for.
struct CpuSetRecord
{
    std::uint32_t id = 0;
    std::uint16_t group = 0;
    std::uint8_t logicalProcessorIndex = 0;
};

/// Group layouts carrying only the maximum counts, for cpuAffinityFromGroupMasks().
[[nodiscard]] inline std::vector<ProcessorGroupLayout> groupLayouts(std::span<const std::uint32_t> groupSizes)
{
    std::vector<ProcessorGroupLayout> layouts;
    layouts.reserve(groupSizes.size());
    for (const std::uint32_t size : groupSizes)
    {
        layouts.push_back({.maximumProcessors = size, .activeMask = 0});
    }
    return layouts;
}

/// The processor-group masks for @p cpus: one entry per group with a chosen CPU, ascending by group.
/// Refused (with the message to show) when @p cpus is empty, when the group sizes are unknown, or
/// when it names a CPU beyond the last group -- a CPU this machine doesn't have.
[[nodiscard]] inline std::expected<std::vector<GroupAffinityMask>, std::string>
groupMasksForAffinity(const CpuAffinity& cpus, std::span<const std::uint32_t> groupSizes)
{
    if (cpus.empty())
    {
        return std::unexpected(std::string("Choose at least one CPU"));
    }
    if (groupSizes.empty())
    {
        return std::unexpected(std::string("The processor groups of this PC couldn't be read, so no CPU affinity was set"));
    }
    std::vector<GroupAffinityMask> masks;
    std::size_t firstCpu = 0;
    std::size_t group = 0;
    while (group < groupSizes.size())
    {
        const std::size_t size = std::min<std::size_t>(groupSizes[group], CpuAffinity::BITS_PER_WORD);
        std::uint64_t mask = 0;
        std::size_t index = 0;
        while (index < size)
        {
            if (cpus.test(firstCpu + index))
            {
                mask |= std::uint64_t{1} << index;
            }
            ++index;
        }
        if (mask != 0)
        {
            masks.push_back({.group = static_cast<std::uint16_t>(group), .mask = mask});
        }
        firstCpu += groupSizes[group];
        ++group;
    }
    // Anything at or past firstCpu, or in a gap past a group's 64th processor, names no processor.
    if (cpuAffinityFromGroupMasks(masks, groupLayouts(groupSizes)) != cpus)
    {
        return std::unexpected(std::string("Some of those CPUs don't exist on this PC, so no CPU affinity was set"));
    }
    return masks;
}

/// The CPU set ids for @p masks' processors, for SetProcessDefaultCpuSets(). Refused when a chosen
/// processor has no CPU set (it went offline since the dialog opened, say), rather than setting a
/// smaller set than was asked for.
[[nodiscard]] inline std::expected<std::vector<std::uint32_t>, std::string> cpuSetIdsFor(std::span<const GroupAffinityMask> masks,
                                                                                         std::span<const CpuSetRecord> cpuSets)
{
    std::vector<std::uint32_t> ids;
    std::size_t wanted = 0;
    for (const GroupAffinityMask& entry : masks)
    {
        wanted += static_cast<std::size_t>(std::popcount(entry.mask));
        for (const CpuSetRecord& set : cpuSets)
        {
            if (set.group == entry.group && set.logicalProcessorIndex < CpuAffinity::BITS_PER_WORD &&
                ((entry.mask >> set.logicalProcessorIndex) & 1U) != 0)
            {
                ids.push_back(set.id);
            }
        }
    }
    if (ids.size() != wanted)
    {
        return std::unexpected(
            std::format("Only {} of the {} CPUs chosen are available to Windows now, so no CPU affinity was set", ids.size(), wanted));
    }
    return ids;
}

/// The Set affinity dialog's layout from the processor groups' sizes and the cores' records: the
/// CPUs online (every core's processors) and, on a hybrid CPU, each one's efficiency class. A core
/// in a group the sizes don't have is skipped. With more than one group the choice is set as CPU
/// sets (see CpuAffinityLayout::appliedAsCpuSets).
[[nodiscard]] inline CpuAffinityLayout affinityLayoutFromCores(std::span<const std::uint32_t> groupSizes,
                                                               std::span<const ProcessorCoreRecord> cores)
{
    CpuAffinityLayout layout;
    layout.groupSizes.assign(groupSizes.begin(), groupSizes.end());
    layout.appliedAsCpuSets = groupSizes.size() > 1;

    std::vector<std::size_t> firstCpu(groupSizes.size(), 0);
    std::size_t index = 1;
    while (index < groupSizes.size())
    {
        firstCpu[index] = firstCpu[index - 1] + groupSizes[index - 1];
        ++index;
    }
    std::vector<GroupAffinityMask> online;
    for (const ProcessorCoreRecord& core : cores)
    {
        if (core.group >= groupSizes.size())
        {
            continue;
        }
        online.push_back({.group = core.group, .mask = core.mask});
        std::uint64_t bits = core.mask;
        while (bits != 0)
        {
            const auto bit = static_cast<std::size_t>(std::countr_zero(bits));
            CpuTopology::setEfficiencyClass(layout.efficiencyClassByCpu, firstCpu[core.group] + bit, core.efficiencyClass);
            bits &= bits - 1; // Clear the lowest set bit
        }
    }
    layout.online = cpuAffinityFromGroupMasks(online, groupLayouts(groupSizes));
    CpuTopology::keepOnlyIfHybrid(layout.efficiencyClassByCpu);
    return layout;
}

} // namespace Platform
