#pragma once

#include "SystemTypes.h"

namespace Platform
{

/// Interface for platform-specific system metrics.
/// Implementations read raw counters from OS APIs.
/// Domain layer computes deltas, rates, and percentages.
class ISystemProbe
{
  public:
    virtual ~ISystemProbe() = default;

    ISystemProbe() = default;
    ISystemProbe(const ISystemProbe&) = default;
    ISystemProbe& operator=(const ISystemProbe&) = default;
    ISystemProbe(ISystemProbe&&) = default;
    ISystemProbe& operator=(ISystemProbe&&) = default;

    /// Returns raw system counters (stateless read).
    [[nodiscard]] virtual SystemCounters read() = 0;

    /// What this platform supports.
    [[nodiscard]] virtual SystemCapabilities capabilities() const = 0;

    /// Clock ticks per second (e.g., sysconf(_SC_CLK_TCK) on Linux).
    [[nodiscard]] virtual long ticksPerSecond() const = 0;
};

} // namespace Platform
