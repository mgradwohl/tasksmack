#pragma once

#include "Platform/ISystemInfoProbe.h"

namespace Platform
{

/// Windows System Information facts (#1399), unprivileged: the OS from the CurrentVersion registry
/// key, the session from the computer-name, join, user-name, locale and time-zone APIs (#1512), and
/// firmware, board and chassis from the SMBIOS table (GetSystemFirmwareTable('RSMB'), #1513), and memory
/// modules from the same table plus installed against usable memory (#1515), and commit and page files
/// (WindowsCommitPaging.h, #1516), and physical disks and volumes (WindowsStorage.h, #1517).
class WindowsSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    [[nodiscard]] SystemInfoCapabilities capabilities() const override;
    [[nodiscard]] OsInfo readOs() override;
    [[nodiscard]] FirmwareInfo readFirmware() override;
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override;
    [[nodiscard]] CommitPagingInfo readCommitPaging() override;
    [[nodiscard]] StorageInfo readStorage() override;
    /// Not read yet: Secure Boot, the TPM, HVCI and Kernel DMA Protection are the Windows lane's
    /// follow-up to #1514, so the section is left out on Windows.
    [[nodiscard]] PlatformSecurityInfo readPlatformSecurity() override
    {
        return {};
    }
    /// Not read yet: the thermal zone counters (PDH "\Thermal Zone Information(*)\Temperature") are the
    /// Windows lane's follow-up to #1522, so the section is left out on Windows.
    [[nodiscard]] SensorsInfo readSensors() override
    {
        return {};
    }
    /// Not read yet: GetAdaptersAddresses and WlanQueryInterface are the Windows lane's follow-up to
    /// #1518, so the section is left out on Windows.
    [[nodiscard]] NetworkAdaptersInfo readNetworkAdapters() override
    {
        return {};
    }
    /// Not read yet: the Diagnostics-Performance event 100 is the Windows lane's follow-up to #1525, so
    /// the section is left out on Windows.
    [[nodiscard]] BootPerformanceInfo readBootPerformance() override
    {
        return {};
    }
};

} // namespace Platform
