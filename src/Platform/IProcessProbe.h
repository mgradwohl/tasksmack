#pragma once

#include "ProcessTypes.h"

#include <vector>

namespace Platform
{

/// Interface for platform-specific process enumeration.
/// Implementations read raw counters from OS APIs.
/// Domain layer computes deltas, rates, and percentages.
class IProcessProbe
{
  public:
    virtual ~IProcessProbe() = default;

    IProcessProbe() = default;
    IProcessProbe(const IProcessProbe&) = default;
    IProcessProbe& operator=(const IProcessProbe&) = default;
    IProcessProbe(IProcessProbe&&) = default;
    IProcessProbe& operator=(IProcessProbe&&) = default;

    /// Returns raw counters for all visible processes (stateless read).
    [[nodiscard]] virtual std::vector<ProcessCounters> enumerate() = 0;

    /// What this platform supports.
    [[nodiscard]] virtual ProcessCapabilities capabilities() const = 0;

    /// Total system CPU time (sum of all cores, all states).
    /// Used for calculating per-process CPU%.
    [[nodiscard]] virtual uint64_t totalCpuTime() const = 0;

    /// Clock ticks per second (e.g., sysconf(_SC_CLK_TCK) on Linux).
    [[nodiscard]] virtual long ticksPerSecond() const = 0;
};

} // namespace Platform
