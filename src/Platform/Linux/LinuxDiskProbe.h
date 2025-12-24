#pragma once

#include "Platform/IDiskProbe.h"

namespace Platform
{

/// Linux implementation of IDiskProbe.
/// Reads disk I/O metrics from /proc/diskstats.
class LinuxDiskProbe : public IDiskProbe
{
  public:
    LinuxDiskProbe();
    ~LinuxDiskProbe() override = default;

    LinuxDiskProbe(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe& operator=(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe(LinuxDiskProbe&&) = default;
    LinuxDiskProbe& operator=(LinuxDiskProbe&&) = default;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

  private:
    bool shouldIncludeDevice(const std::string& deviceName) const;
};

} // namespace Platform
