#pragma once

#include "Platform/ISystemInfoProbe.h"

#include <filesystem>
#include <utility>

namespace Platform
{

/// Linux System Information facts (#1399), unprivileged: the distro, init, boot time, time zone and
/// container/VM hints from files under the root (LinuxOsInfo.h), plus uname, the host name, the user
/// and the session's XDG and locale environment (#1512); firmware, board and chassis from
/// /sys/class/dmi/id (LinuxFirmwareInfo.h, #1513); memory modules from the root-only SMBIOS table and
/// the usable total from /proc/meminfo (#1515); commit, swap, zram, zswap and huge pages from /proc and
/// /sys (LinuxCommitPaging.h, #1516); disks from /sys/block and volumes from /proc/self/mountinfo and
/// statvfs (LinuxStorage.h, #1517); Secure Boot, the TPM, security modules, lockdown and CPU
/// vulnerabilities from /sys (LinuxPlatformSecurity.h, #1514); hwmon and thermal-zone sensors
/// (LinuxSensors.h, #1522); GPUs and monitors from /sys/class/drm and the display server from the
/// session environment (LinuxGraphics.h, #1519); adapters, gateways and DNS from /sys, /proc and
/// resolv.conf, addresses from getifaddrs() (LinuxNetworkAdapters.h, #1518); boot phases from systemd
/// over D-Bus when built with libsystemd (SystemdBus.h, LinuxBootTimes.h, #1525); PCI and USB devices
/// from /sys/bus and audio from /proc/asound (LinuxDevices.h, #1520); loaded kernel modules from /proc/modules
/// (LinuxKernelModules.h, #1521).
class LinuxSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    /// @param root Where /etc, /proc, /run and /sys are read from; tests point it at a fixture tree.
    /// Volumes are sized by statvfs() on their mount points as the running system sees them.
    explicit LinuxSystemInfoProbe(std::filesystem::path root = "/") : m_Root(std::move(root))
    {}

    [[nodiscard]] SystemInfoCapabilities capabilities() const override;
    [[nodiscard]] OsInfo readOs() override;
    [[nodiscard]] FirmwareInfo readFirmware() override;
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override;
    [[nodiscard]] CommitPagingInfo readCommitPaging() override;
    [[nodiscard]] StorageInfo readStorage() override;
    [[nodiscard]] GraphicsInfo readGraphics() override;
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override;
    [[nodiscard]] SensorsInfo readSensors() override;
    [[nodiscard]] DevicesInfo readDevices() override;
    [[nodiscard]] DriversInfo readDrivers() override;
    [[nodiscard]] NetworkAdaptersInfo readNetworkAdapters() override;
    [[nodiscard]] BootPerformanceInfo readBootPerformance() override;

  private:
    std::filesystem::path m_Root;
};

} // namespace Platform
