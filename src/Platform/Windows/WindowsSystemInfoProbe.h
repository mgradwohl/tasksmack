#pragma once

#include "Platform/ISystemInfoProbe.h"
#include "Platform/Windows/WindowsDevices.h"
#include "Platform/Windows/WindowsDrivers.h"
#include "Platform/Windows/WindowsGraphics.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // not <lm.h>: its lmerrlog.h HLOG clashes with pdh.h's, and Factory.cpp sees both
// clang-format on

namespace Platform
{

/// The Win32 calls readOs(), readFirmware() and readMemoryModules() make (#1722): the system's by default
/// (WindowsSystemInfoProbe::systemFunctions()), fakes in tests, so each failure can be shown to leave only
/// its own field empty. Every member must be set.
struct SystemInfoFunctions
{
    // The OS and session (readOs).
    LSTATUS(WINAPI* regGetValue)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD) = nullptr;
    BOOL(WINAPI* isWow64Process2)(HANDLE, USHORT*, USHORT*) = nullptr;
    BOOL (*computerName)(LPWSTR, PULONG) = nullptr; ///< GetComputerNameExW(ComputerNameDnsHostname, ...)
    BOOL (*userName)(LPWSTR, PULONG) = nullptr;     ///< GetUserNameExW(NameSamCompatible, ...)
    /// NetGetJoinInformation, with NET_API_STATUS as DWORD and the NETSETUP_JOIN_STATUS out-param as a DWORD,
    /// so this header needs no <lm.h> (the .cpp static_asserts the types match).
    DWORD (*netGetJoinInformation)(LPCWSTR server, LPWSTR* name, DWORD* joinStatus) = nullptr;
    DWORD(WINAPI* netApiBufferFree)(LPVOID) = nullptr; ///< NetApiBufferFree (NET_API_STATUS is DWORD)
    int(WINAPI* getUserDefaultLocaleName)(LPWSTR, int) = nullptr;
    DWORD(WINAPI* getDynamicTimeZoneInformation)(PDYNAMIC_TIME_ZONE_INFORMATION) = nullptr;
    UINT(WINAPI* getSystemDirectory)(LPWSTR, UINT) = nullptr;
    UINT(WINAPI* getSystemWindowsDirectory)(LPWSTR, UINT) = nullptr;

    // Firmware and the SMBIOS table (readFirmware, readMemoryModules).
    BOOL(WINAPI* getFirmwareType)(PFIRMWARE_TYPE) = nullptr;
    UINT(WINAPI* getSystemFirmwareTable)(DWORD, DWORD, PVOID, DWORD) = nullptr;
    POWER_PLATFORM_ROLE(WINAPI* powerDeterminePlatformRoleEx)(ULONG) = nullptr;
};

/// Windows System Information facts (#1399), unprivileged: the OS from the CurrentVersion registry
/// key, the session from the computer-name, join, user-name, locale and time-zone APIs (#1512), and
/// firmware, board and chassis from the SMBIOS table (GetSystemFirmwareTable('RSMB'), #1513), and memory
/// modules from the same table plus installed against usable memory (#1515), and commit and page files
/// (WindowsCommitPaging.h, #1516), and physical disks and volumes (WindowsStorage.h, #1517), and GPUs,
/// drivers and monitors from DXGI, SetupAPI and EDID (WindowsGraphics.h, #1519), and device nodes, their
/// problems and audio endpoints from SetupAPI, CfgMgr32 and MMDevice (WindowsDevices.h, #1520), and the running
/// driver services from the Service Control Manager with their images' versions (WindowsDrivers.h, #1521), the
/// drivers that failed to start and the images' signatures (#1661), and
/// the Application log's crash and hang events (WindowsCrashEvents.h, #1524).
class WindowsSystemInfoProbe final : public ISystemInfoProbe
{
  public:
    WindowsSystemInfoProbe();
    /// Make the OS, session and firmware calls through @p functions rather than the system's (tests, #1722).
    explicit WindowsSystemInfoProbe(const SystemInfoFunctions& functions);

    /// The real calls.
    [[nodiscard]] static SystemInfoFunctions systemFunctions();
    /// The real readers readGraphics(), readDevices() and readDrivers() compose, for tests to call
    /// directly with inputs that fail (#1722).
    [[nodiscard]] static WindowsGraphics::Functions graphicsFunctions();
    [[nodiscard]] static WindowsDevices::Functions deviceFunctions();
    [[nodiscard]] static WindowsDrivers::Functions driverFunctions();

    [[nodiscard]] SystemInfoCapabilities capabilities() const override;
    [[nodiscard]] OsInfo readOs() override;
    [[nodiscard]] FirmwareInfo readFirmware() override;
    [[nodiscard]] MemoryModulesInfo readMemoryModules() override;
    [[nodiscard]] CommitPagingInfo readCommitPaging() override;
    [[nodiscard]] StorageInfo readStorage() override;
    [[nodiscard]] GraphicsInfo readGraphics() override;
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
    [[nodiscard]] DevicesInfo readDevices() override;
    [[nodiscard]] DriversInfo readDrivers() override;
    [[nodiscard]] CrashesInfo readCrashes() override;
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

  private:
    SystemInfoFunctions m_Fns;
    WindowsDrivers::SignatureCache m_SignatureCache; ///< Driver images' signatures by path and file time (#1661)
};

} // namespace Platform
