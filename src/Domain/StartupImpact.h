#pragma once

// A startup entry's Startup impact rating (#3073), as Task Manager rates it, from what its program cost
// at the last measured sign-in (Platform::measureStartupProgram). Pure: no clock, no I/O.
//
// The thresholds are Microsoft's, as Task Manager's documentation states them (support article
// KB5022092, "Startup impact" in Task Manager): Low is under 300 ms of CPU time and under about 300 KB
// of disk I/O (the article says 292 KB, which is 300,000 bytes in binary kilobytes); High is more than
// 1 s of CPU time or more than 3 MB of disk I/O; Medium is everything between. TaskSmack takes the
// disk figures as 300,000 and 3,000,000 bytes. A disabled entry is "None", as Task Manager shows it: it
// doesn't start at sign-in, whatever it cost before.

#include "Platform/IStartupProbe.h"
#include "Platform/StartupImpact.h"

#include <cstdint>

namespace Domain
{

/// The Startup impact column's rating, ordered from no impact to the most.
enum class StartupImpact : std::uint8_t
{
    None,        ///< Disabled: it doesn't start at sign-in.
    NotMeasured, ///< Enabled, but the last measured sign-in has no figures for its program.
    Low,
    Medium,
    High,
};

/// Below both of these (CPU time, disk I/O) is Low.
inline constexpr std::uint64_t LOW_IMPACT_CPU_US = 300'000;
inline constexpr std::uint64_t LOW_IMPACT_DISK_BYTES = 300'000;
/// Above either of these is High.
inline constexpr std::uint64_t HIGH_IMPACT_CPU_US = 1'000'000;
inline constexpr std::uint64_t HIGH_IMPACT_DISK_BYTES = 3'000'000;

/// The rating for a program that used @p cpuUs of CPU time and @p diskBytes of disk I/O at sign-in.
[[nodiscard]] constexpr StartupImpact rateStartupImpact(std::uint64_t cpuUs, std::uint64_t diskBytes) noexcept
{
    if (cpuUs > HIGH_IMPACT_CPU_US || diskBytes > HIGH_IMPACT_DISK_BYTES)
    {
        return StartupImpact::High;
    }
    if (cpuUs < LOW_IMPACT_CPU_US && diskBytes < LOW_IMPACT_DISK_BYTES)
    {
        return StartupImpact::Low;
    }
    return StartupImpact::Medium;
}

/// One entry's impact: its rating and, when measured, the figures it was rated from.
struct StartupEntryImpact
{
    std::uint64_t cpuUs = 0;
    std::uint64_t diskBytes = 0;
    std::uint32_t processCount = 0; ///< 0 when not measured.
    StartupImpact rating = StartupImpact::NotMeasured;
};

/// @p entry's impact at the sign-in @p reading measured: None when it is disabled, NotMeasured when its
/// program is unknown or didn't run then, else rated from what its processes cost.
[[nodiscard]] inline StartupEntryImpact startupEntryImpact(const Platform::StartupEntry& entry,
                                                           const Platform::StartupImpactReading& reading)
{
    if (!entry.enabled)
    {
        return {.rating = StartupImpact::None};
    }
    const Platform::StartupImpactTotals totals = Platform::measureStartupProgram(reading, entry.executablePath);
    if (totals.processCount == 0)
    {
        return {};
    }
    return {.cpuUs = totals.cpuUs,
            .diskBytes = totals.diskBytes,
            .processCount = totals.processCount,
            .rating = rateStartupImpact(totals.cpuUs, totals.diskBytes)};
}

} // namespace Domain
