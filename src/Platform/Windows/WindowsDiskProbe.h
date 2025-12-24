#pragma once

#include "Platform/IDiskProbe.h"

namespace Platform
{

/// Windows implementation of IDiskProbe.
/// Reads disk I/O metrics from Windows performance counters.
class WindowsDiskProbe : public IDiskProbe
{
  public:
    WindowsDiskProbe();
    ~WindowsDiskProbe() override = default;

    WindowsDiskProbe(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe& operator=(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe(WindowsDiskProbe&&) = default;
    WindowsDiskProbe& operator=(WindowsDiskProbe&&) = default;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;
};

} // namespace Platform
