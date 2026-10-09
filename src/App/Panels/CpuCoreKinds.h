#pragma once

// Which kind of core each CPU Cores chart is on a hybrid CPU: performance, efficiency or low-power
// efficiency (#1536). Pure, so it is unit-testable without an ImGui context (CONTRIBUTING.md's
// "extract the pure decision logic into a small header").

#include "Platform/CpuDetails.h"

#include <algorithm>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace App::CpuCoresSection
{

/// The marker a core's chart carries. None on a CPU with one kind of core, or for a core whose
/// efficiency class is unknown.
enum class CoreKind : std::uint8_t
{
    None,
    Performance, ///< The highest efficiency class.
    Efficiency,  ///< Any class between the highest and, with three or more classes, the lowest.
    LowPower,    ///< The lowest class, when there are three or more (e.g. an SoC tile's LP E-cores).
};

/// Maps each core id's efficiency class (CpuDetails::efficiencyClassByCoreId, higher is more
/// performant) to its marker, indexed the same way. Fewer than two known classes -- a homogeneous
/// CPU, or one the platform cannot tell -- leaves `out` empty: no core gets a marker.
/// `out` is reused, so a caller that keeps it across snapshots doesn't allocate once it has grown.
inline void classifyCoreKinds(const std::vector<std::uint8_t>& efficiencyClasses, std::vector<CoreKind>& out)
{
    out.clear();
    bool any = false;
    std::uint8_t lowest = 0;
    std::uint8_t highest = 0;
    std::size_t distinct = 0;
    std::bitset<256> seen; // Counts the distinct classes without sorting a copy
    for (const std::uint8_t cls : efficiencyClasses)
    {
        if (cls == Platform::UNKNOWN_EFFICIENCY_CLASS)
        {
            continue;
        }
        if (!seen.test(cls))
        {
            seen.set(cls);
            ++distinct;
        }
        lowest = any ? std::min(lowest, cls) : cls;
        highest = any ? std::max(highest, cls) : cls;
        any = true;
    }
    if (distinct < 2)
    {
        return;
    }
    out.reserve(efficiencyClasses.size());
    for (const std::uint8_t cls : efficiencyClasses)
    {
        if (cls == Platform::UNKNOWN_EFFICIENCY_CLASS)
        {
            out.push_back(CoreKind::None);
        }
        else if (cls == highest)
        {
            out.push_back(CoreKind::Performance);
        }
        else if (distinct >= 3 && cls == lowest)
        {
            out.push_back(CoreKind::LowPower);
        }
        else
        {
            out.push_back(CoreKind::Efficiency);
        }
    }
}

/// The marker for `coreId`: None past the end of `kinds` (a homogeneous CPU has none at all).
[[nodiscard]] inline CoreKind coreKindFor(const std::vector<CoreKind>& kinds, std::size_t coreId) noexcept
{
    return coreId < kinds.size() ? kinds[coreId] : CoreKind::None;
}

/// What the marker's tooltip says; empty for None. A C string, as ImGui takes it.
[[nodiscard]] constexpr const char* coreKindDescription(CoreKind kind) noexcept
{
    switch (kind)
    {
    case CoreKind::Performance:
        return "Performance core";
    case CoreKind::Efficiency:
        return "Efficiency core";
    case CoreKind::LowPower:
        return "Low-power efficiency core";
    case CoreKind::None:
        break;
    }
    return "";
}

} // namespace App::CpuCoresSection
