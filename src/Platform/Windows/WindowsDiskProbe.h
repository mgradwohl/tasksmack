#pragma once

#include "Platform/IDiskProbe.h"
#include "Platform/StorageTypes.h"

#include <memory>

namespace Platform
{

/// Windows implementation of IDiskProbe. Uses IOCTL_DISK_PERFORMANCE for cumulative
/// disk I/O counters; Performance Data Helper (PDH) is used only to enumerate physical disk
/// instance names (e.g. "0 C:"), at construction and again from read() after a disk fails or
/// every DISK_REENUMERATE_INTERVAL, so removed and added disks are picked up (#1159).
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
    /// The disks' counters, without rescanning; read() rescans afterwards when it is due (#1159).
    [[nodiscard]] SystemDiskCounters readCounters();

    // Opaque implementation to avoid including Windows headers in public header
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

} // namespace Platform
