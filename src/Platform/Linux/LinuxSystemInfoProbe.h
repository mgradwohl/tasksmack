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
/// vulnerabilities from /sys (LinuxPlatformSecurity.h, #1514).
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
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override;

  private:
    std::filesystem::path m_Root;
};

} // namespace Platform
