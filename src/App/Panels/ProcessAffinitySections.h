#pragma once

// How the Set affinity dialog groups its CPU boxes (#1881): by processor group (Windows, more than
// one) and, on a hybrid CPU, by kind of core. Pure, so it is unit-testable without an ImGui context.

#include "App/Panels/CpuCoreKinds.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace App
{

/// One run of boxes in the dialog, under its heading.
struct AffinityDialogSection
{
    std::string title;             ///< "Group 1: Performance cores", say; empty when it is the only section
    std::vector<std::size_t> cpus; ///< Ascending
};

namespace AffinitySectionsDetail
{
/// Section order within a processor group: performance cores first, as CPU Cores lists them.
[[nodiscard]] constexpr int coreKindOrder(CpuCoresSection::CoreKind kind) noexcept
{
    switch (kind)
    {
    case CpuCoresSection::CoreKind::Performance:
        return 0;
    case CpuCoresSection::CoreKind::Efficiency:
        return 1;
    case CpuCoresSection::CoreKind::LowPower:
        return 2;
    case CpuCoresSection::CoreKind::None:
        break;
    }
    return 3;
}

/// A section's heading for a kind of core; None (a CPU with no reading on a hybrid CPU) is "Other CPUs".
[[nodiscard]] constexpr const char* coreKindHeading(CpuCoresSection::CoreKind kind) noexcept
{
    switch (kind)
    {
    case CpuCoresSection::CoreKind::Performance:
        return "Performance cores";
    case CpuCoresSection::CoreKind::Efficiency:
        return "Efficiency cores";
    case CpuCoresSection::CoreKind::LowPower:
        return "Low-power efficiency cores";
    case CpuCoresSection::CoreKind::None:
        break;
    }
    return "Other CPUs";
}
} // namespace AffinitySectionsDetail

/// The CPUs the dialog offers, ascending: @p layout's online CPUs when known, otherwise 0 to
/// @p cpuCount - 1; in either case with every CPU @p current names, so a CPU that went offline keeps
/// its box. At least CPU 0.
[[nodiscard]] inline std::vector<std::size_t>
affinityDialogOfferedCpus(const Platform::CpuAffinity& current, std::size_t cpuCount, const Platform::CpuAffinityLayout& layout)
{
    Platform::CpuAffinity offered = layout.online;
    if (offered.empty() && cpuCount > 0)
    {
        offered.setRange(0, cpuCount - 1);
    }
    std::vector<std::size_t> cpus;
    const auto collect = [&cpus](const Platform::CpuAffinity& affinity)
    {
        const auto words = affinity.words();
        std::size_t word = 0;
        while (word < words.size())
        {
            std::uint64_t bits = words[word];
            while (bits != 0)
            {
                cpus.push_back((word * Platform::CpuAffinity::BITS_PER_WORD) + static_cast<std::size_t>(std::countr_zero(bits)));
                bits &= bits - 1; // Clear the lowest set bit
            }
            ++word;
        }
    };
    collect(offered);
    collect(current);
    std::ranges::sort(cpus);
    const auto [first, last] = std::ranges::unique(cpus);
    cpus.erase(first, last);
    if (cpus.empty())
    {
        cpus.push_back(0);
    }
    return cpus;
}

/// @p cpus (ascending) in the dialog's sections: one per processor group when @p layout has more than
/// one, split by kind of core on a hybrid CPU, performance cores first. A CPU past the last group goes
/// in "Other CPUs"; on a hybrid CPU, so does one of no known kind (within its group). A single
/// section has no title.
[[nodiscard]] inline std::vector<AffinityDialogSection> affinityDialogSections(const std::vector<std::size_t>& cpus,
                                                                               const Platform::CpuAffinityLayout& layout)
{
    using CpuCoresSection::CoreKind;
    std::vector<CoreKind> kinds;
    CpuCoresSection::classifyCoreKinds(layout.efficiencyClassByCpu, kinds);
    const std::size_t groupCount = layout.groupSizes.size();
    const bool grouped = groupCount > 1;
    const bool hybrid = !kinds.empty();

    struct Keyed
    {
        std::size_t group = 0; // groupCount: past the last group
        CoreKind kind = CoreKind::None;
        std::size_t cpu = 0;
    };
    std::vector<Keyed> keyed;
    keyed.reserve(cpus.size());
    for (const std::size_t cpu : cpus)
    {
        std::size_t group = 0;
        if (grouped)
        {
            std::size_t firstCpu = 0;
            while (group < groupCount && cpu >= firstCpu + layout.groupSizes[group])
            {
                firstCpu += layout.groupSizes[group];
                ++group;
            }
        }
        const bool pastLastGroup = grouped && group >= groupCount; // All "Other CPUs", whatever their kind
        keyed.push_back(
            {.group = group, .kind = (hybrid && !pastLastGroup) ? CpuCoresSection::coreKindFor(kinds, cpu) : CoreKind::None, .cpu = cpu});
    }
    std::ranges::stable_sort(keyed,
                             [](const Keyed& a, const Keyed& b)
                             {
                                 if (a.group != b.group)
                                 {
                                     return a.group < b.group;
                                 }
                                 return AffinitySectionsDetail::coreKindOrder(a.kind) < AffinitySectionsDetail::coreKindOrder(b.kind);
                             });

    std::vector<AffinityDialogSection> sections;
    const Keyed* previous = nullptr;
    for (const Keyed& entry : keyed)
    {
        if (previous == nullptr || entry.group != previous->group || entry.kind != previous->kind)
        {
            std::string title;
            if (grouped && entry.group >= groupCount)
            {
                title = "Other CPUs";
            }
            else if (grouped && hybrid)
            {
                title = std::format("Group {}: {}", entry.group, AffinitySectionsDetail::coreKindHeading(entry.kind));
            }
            else if (grouped)
            {
                title = std::format("Group {}", entry.group);
            }
            else if (hybrid)
            {
                title = AffinitySectionsDetail::coreKindHeading(entry.kind);
            }
            sections.push_back({.title = std::move(title), .cpus = {}});
        }
        sections.back().cpus.push_back(entry.cpu);
        previous = &entry;
    }
    if (sections.size() == 1)
    {
        sections.front().title.clear();
    }
    return sections;
}

} // namespace App
