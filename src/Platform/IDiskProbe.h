#pragma once

#include "StorageTypes.h"

namespace Platform
{

/// Interface for platform-specific disk/storage metrics.
/// Implementations read raw counters from OS APIs.
/// Domain layer computes deltas, rates, and percentages.
class IDiskProbe
{
  public:
    virtual ~IDiskProbe() = default;

    IDiskProbe() = default;
    IDiskProbe(const IDiskProbe&) = default;
    IDiskProbe& operator=(const IDiskProbe&) = default;
    IDiskProbe(IDiskProbe&&) = default;
    IDiskProbe& operator=(IDiskProbe&&) = default;

    /// Returns raw disk I/O counters for all devices (stateless read).
    [[nodiscard]] virtual SystemDiskCounters read() = 0;

    /// What this platform supports.
    [[nodiscard]] virtual DiskCapabilities capabilities() const = 0;
};

} // namespace Platform
