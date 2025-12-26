#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <memory>

namespace Platform
{

/// Windows implementation of IDiskProbe using Performance Data Helper (PDH).
/// Reads disk I/O metrics from Windows Performance Counters.
class WindowsDiskProbe : public IDiskProbe
{
  public:
    WindowsDiskProbe();
    ~WindowsDiskProbe() override;

    WindowsDiskProbe(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe& operator=(const WindowsDiskProbe&) = delete;
    WindowsDiskProbe(WindowsDiskProbe&&) = delete;
    WindowsDiskProbe& operator=(WindowsDiskProbe&&) = delete;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

  private:
    // Opaque implementation to avoid including Windows headers in public header
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
