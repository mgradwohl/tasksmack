#pragma once

#include "Platform/IDiskProbe.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

    /// Testability constructor with a stand-in for /sys/block too, whose entries are the whole block devices (#4391).
    LinuxDiskProbe(std::filesystem::path diskstatsPath, std::filesystem::path sysBlockPath);

    ~LinuxDiskProbe() override = default;

    LinuxDiskProbe(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe& operator=(const LinuxDiskProbe&) = delete;
    LinuxDiskProbe(LinuxDiskProbe&&) noexcept = default;
    LinuxDiskProbe& operator=(LinuxDiskProbe&&) noexcept = default;

    [[nodiscard]] SystemDiskCounters read() override;
    [[nodiscard]] DiskCapabilities capabilities() const override;

    /// One entry of /sys/block: a whole block device, and whether it is stacked on others (#4391).
    struct WholeDevice
    {
        std::string name;
        /// md, bcache (a non-empty slaves/) or a ZFS zvol (zd*): its I/O also reaches the member disks, so it is shown
        /// but kept out of the totals, which would otherwise count it twice.
        bool stacked = false;
    };

    /// The whole block devices under @p sysBlockPath, or nullopt when it can't be listed.
    [[nodiscard]] static std::optional<std::vector<WholeDevice>> listWholeDevices(const std::filesystem::path& sysBlockPath);

    /// How long a /sys/block listing is used before it is made again with no change in /proc/diskstats' device names
    /// (#4697); Domain::Sampling::DISK_DEVICE_RESCAN_INTERVAL_SECONDS. Zero lists it every sample, for tests.
    void setRescanIntervalForTesting(std::chrono::steady_clock::duration interval) noexcept
    {
        m_RescanInterval = interval;
    }

  private:
    /// The name rule, for when /sys/block can't be listed: a name ending in a digit is a partition unless it is an
    /// NVMe namespace or an eMMC disk.
    [[nodiscard]] static bool shouldIncludeDevice(const std::string& deviceName);
    /// Loop devices, RAM disks, zram (swap in memory), device-mapper volumes, optical drives, floppies and an eMMC's
    /// boot and RPMB areas are left out either way (#4628).
    [[nodiscard]] static bool isAlwaysExcluded(const std::string& deviceName);
    /// Whether the /sys/block entry @p deviceDir is a device with no media: size 0 (#4628).
    [[nodiscard]] static bool hasNoMedia(const std::filesystem::path& deviceDir);

    /// The /sys/block listing to use for this sample (#4697): the last one while /proc/diskstats names the same devices
    /// and the interval hasn't passed, else a new one. Nullopt when there's no /sys/block to list.
    [[nodiscard]] const std::optional<std::vector<WholeDevice>>& wholeDevicesFor(const std::vector<std::string>& diskstatsNames);

    std::filesystem::path m_DiskstatsPath;
    std::filesystem::path m_SysBlockPath; ///< Empty: the name rule alone (the one-argument test constructor)
    // The last /sys/block listing (#4697): each device's size and slaves/ cost a sysfs open apiece, about 3x the read.
    std::optional<std::vector<WholeDevice>> m_WholeDevices;
    std::vector<std::string> m_ListedFor; ///< /proc/diskstats' device names when it was made
    std::chrono::steady_clock::time_point m_ListedAt;
    bool m_Listed = false;
    std::chrono::steady_clock::duration m_RescanInterval; ///< DISK_DEVICE_RESCAN_INTERVAL_SECONDS, set by the constructors
};

} // namespace Platform
