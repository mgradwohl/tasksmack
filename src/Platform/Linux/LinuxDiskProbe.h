#pragma once

#include "Platform/IDiskProbe.h"

#include <filesystem>

namespace Platform
{

/// Linux implementation of IDiskProbe.
/// Reads disk I/O metrics from /proc/diskstats.
class LinuxDiskProbe : public IDiskProbe
{
  public:
    LinuxDiskProbe();

    /// Testability constructor: reads from a custom diskstats path instead of
    /// /proc/diskstats. Useful for unit tests that supply synthetic content.
    explicit LinuxDiskProbe(std::filesystem::path diskstatsPath);

    ~LinuxDiskProbe() override = default;

    LinuxDiskProbe(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe& operator=(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe(LinuxDiskProbe&&) noexcept = default;
    LinuxDiskProbe& operator=(LinuxDiskProbe&&) noexcept = default;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

    [[nodiscard]] static bool shouldIncludeDevice(const std::string& deviceName);

  private:
    std::filesystem::path m_DiskstatsPath;
};

} // namespace Platform
