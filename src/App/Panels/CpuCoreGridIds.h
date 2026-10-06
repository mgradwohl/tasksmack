#pragma once

// Which core ids the CPU Cores grid draws a chart for (#1262). Pure, so it is unit-testable without
// an ImGui context (CONTRIBUTING.md's "extract the pure decision logic into a small header").

#include <cstddef>
#include <vector>

namespace App::CpuCoresSection
{

/// Fills `out` with the core ids to chart, in order: the ids the probe has reported this session
/// (SystemSnapshot::seenCoreIds), not every slot up to the highest id, so a Windows group's reserved
/// hot-add capacity or a Linux cpuN never online gets no permanently empty chart. A seen CPU that has
/// gone offline stays in the list and keeps its chart, with a gap (#1229). With no seen ids -- a
/// snapshot not built by SystemModel -- every one of `slotCount` slots is charted, as before.
/// `out` is reused, so a caller that keeps it across frames doesn't allocate once it has grown.
inline void selectCpuCoreGridIds(const std::vector<std::size_t>& seenCoreIds, std::size_t slotCount, std::vector<std::size_t>& out)
{
    if (!seenCoreIds.empty())
    {
        out.assign(seenCoreIds.begin(), seenCoreIds.end());
        return;
    }
    out.clear();
    for (std::size_t id = 0; id < slotCount; ++id)
    {
        out.push_back(id);
    }
}

/// Whether the CPU Cores tab is shown: when more than one CPU has been reported this session, so a
/// two-CPU machine that drops to one online CPU keeps the tab and the offline CPU's chart with its
/// gap. With no seen ids (a snapshot not built by SystemModel), when the snapshot counts more than one.
[[nodiscard]] inline bool showCpuCoresTab(const std::vector<std::size_t>& seenCoreIds, std::size_t coreCount)
{
    return seenCoreIds.empty() ? coreCount > 1 : seenCoreIds.size() > 1;
}

} // namespace App::CpuCoresSection
