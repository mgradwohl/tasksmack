#pragma once

// One rule for a battery's combined capacity rows (#2409), shared by the Linux and Windows power
// probes so the Overview's Design capacity, Full charge, Wear and Health describe the same packs and
// are computed the same way on both. Before, Windows summed design and full charge over every pack
// but health over the packs reporting both, and Linux truncated health without capping it, so the
// rows could contradict each other (Wear 43.8% next to Health 90%, or Health 104% next to Wear 0%).
//
// The rule:
// - A pack counts toward the capacity rows when it reports both a design and a full-charge capacity
//   (> 0). Design and Full charge are the sums over exactly those packs, and Health comes from those
//   sums: round(min(full / design, 1) * 100). So Health = 100 - Wear, rounded, whenever both show.
// - When no pack reports both, Design and Health are unknown and Full charge is the sum of the
//   packs that report one, so a battery without a design figure still shows its full charge.
//
// Pure and header-only, so both probes and every platform's tests use the same code.

#include <algorithm>
#include <cmath>
#include <span>

namespace Platform
{

/// Battery health: full-charge over design capacity, in the same unit, as a percentage rounded to
/// the nearest and capped at 100 (a new battery can hold a little more than its design). -1 unless
/// both are known (> 0).
[[nodiscard]] inline int batteryHealthPercent(double fullCharge, double design) noexcept
{
    if (!(design > 0.0) || !(fullCharge > 0.0))
    {
        return -1;
    }
    return static_cast<int>(std::lround(std::min(fullCharge / design, 1.0) * 100.0));
}

/// One pack's capacities, in a unit every pack being combined shares; 0 when not reported.
struct PackCapacity
{
    double fullCharge = 0.0;
    double design = 0.0;
};

/// The packs' capacity rows combined by the rule above.
struct CombinedCapacity
{
    double fullCharge = 0.0; ///< 0 when no pack reports one
    double design = 0.0;     ///< 0 unless some pack reports both
    int healthPercent = -1;  ///< batteryHealthPercent(fullCharge, design)
};

/// The packs combined by the rule at the top of this file (#2409).
[[nodiscard]] inline CombinedCapacity combinePackCapacities(std::span<const PackCapacity> packs) noexcept
{
    CombinedCapacity paired;
    double unpairedFull = 0.0;
    for (const PackCapacity& pack : packs)
    {
        if (pack.fullCharge > 0.0 && pack.design > 0.0)
        {
            paired.fullCharge += pack.fullCharge;
            paired.design += pack.design;
        }
        else if (pack.fullCharge > 0.0)
        {
            unpairedFull += pack.fullCharge;
        }
    }
    if (paired.design > 0.0)
    {
        paired.healthPercent = batteryHealthPercent(paired.fullCharge, paired.design);
        return paired;
    }
    return CombinedCapacity{.fullCharge = unpairedFull, .design = 0.0, .healthPercent = -1};
}

} // namespace Platform
