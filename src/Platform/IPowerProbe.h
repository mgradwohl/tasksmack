#pragma once

#include "PowerTypes.h"

namespace Platform
{

/// Interface for platform-specific power/battery metrics.
/// Implementations read raw counters from OS APIs.
/// Domain layer may compute trends or additional derived values.
class IPowerProbe
{
  public:
    virtual ~IPowerProbe() = default;

    IPowerProbe() = default;
    IPowerProbe(const IPowerProbe&) = default;
    IPowerProbe& operator=(const IPowerProbe&) = default;
    IPowerProbe(IPowerProbe&&) = default;
    IPowerProbe& operator=(IPowerProbe&&) = default;

    /// Returns raw power/battery counters (stateless read).
    [[nodiscard]] virtual PowerCounters read() = 0;

    /// What this platform supports.
    [[nodiscard]] virtual PowerCapabilities capabilities() const = 0;
};

} // namespace Platform
