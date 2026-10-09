#include "WindowsSystemInfoProbe.h"

#include "Platform/ISystemInfoProbe.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>
#include <lm.h>
#include <security.h>
#include <powerbase.h> // PowerDeterminePlatformRoleEx (powrprof)
// clang-format on

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "powrprof.lib")
#pragma comment(lib, "secur32.lib")

#include "Platform/SmbiosParser.h"
#include "WinString.h"
#include "WindowsOsInfoMath.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

namespace
{

constexpr const wchar_t* CURRENT_VERSION_KEY = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

/// A REG_SZ value of the CurrentVersion key (64-bit view), UTF-8; empty when absent.
[[nodiscard]] std::string readVersionString(const wchar_t* name)
{
    DWORD bytes = 0;
    constexpr DWORD FLAGS = RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, FLAGS, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
    {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, FLAGS, nullptr, value.data(), &bytes) != ERROR_SUCCESS)
    {
        return {};
    }
    value.resize(bytes / sizeof(wchar_t));
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return WinString::wideToUtf8(value);
}

/// A REG_DWORD value of the CurrentVersion key; nullopt when absent.
[[nodiscard]] std::optional<std::uint32_t> readVersionDword(const wchar_t* name) noexcept
{
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, name, RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, nullptr, &value, &bytes) !=
        ERROR_SUCCESS)
    {
        return std::nullopt;
    }
    return value;
}

/// GetComputerNameExW / GetUserNameExW: on success `size` is the length; on ERROR_MORE_DATA it is the
/// size needed (terminator included), and the call is retried once with that much room.
template<typename Fill> [[nodiscard]] std::string readSizedString(Fill fill)
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        auto size = static_cast<ULONG>(buffer.size());
        if (fill(buffer.data(), size) != FALSE)
        {
            buffer.resize(size);
            return WinString::wideToUtf8(buffer);
        }
        if (GetLastError() != ERROR_MORE_DATA || size <= buffer.size())
        {
            return {};
        }
        buffer.resize(size);
    }
    return {};
}

/// GetSystemDirectoryW / GetSystemWindowsDirectoryW: the length on success, 0 on failure.
template<typename Get> [[nodiscard]] std::string readDirectory(Get get)
{
    std::wstring buffer(MAX_PATH, L'\0');
    UINT length = get(buffer.data(), static_cast<UINT>(buffer.size()));
    if (length > buffer.size())
    {
        buffer.resize(length);
        length = get(buffer.data(), static_cast<UINT>(buffer.size()));
    }
    if (length == 0 || length > buffer.size())
    {
        return {};
    }
    buffer.resize(length);
    return WinString::wideToUtf8(buffer);
}

void readJoinInformation(OsInfo& info)
{
    LPWSTR name = nullptr;
    NETSETUP_JOIN_STATUS status = NetSetupUnknownStatus;
    if (NetGetJoinInformation(nullptr, &name, &status) != NERR_Success)
    {
        return;
    }
    if (name != nullptr && (status == NetSetupDomainName || status == NetSetupWorkgroupName))
    {
        info.domainOrWorkgroup = WinString::wideToUtf8(name);
        info.joinedToDomain = status == NetSetupDomainName;
    }
    NetApiBufferFree(name);
}

void readTimeZone(OsInfo& info)
{
    DYNAMIC_TIME_ZONE_INFORMATION zone{};
    const DWORD id = GetDynamicTimeZoneInformation(&zone);
    if (id == TIME_ZONE_ID_INVALID)
    {
        return;
    }
    // Bias is minutes *west* of UTC; the daylight or standard bias adds to it while in effect.
    LONG bias = zone.Bias;
    if (id == TIME_ZONE_ID_DAYLIGHT)
    {
        bias += zone.DaylightBias;
    }
    else if (id == TIME_ZONE_ID_STANDARD)
    {
        bias += zone.StandardBias;
    }
    info.utcOffsetMinutes = static_cast<int>(-bias);
    info.timeZone = WinString::wideToUtf8(zone.TimeZoneKeyName[0] != L'\0' ? zone.TimeZoneKeyName : zone.StandardName);
}

} // namespace

SystemInfoCapabilities WindowsSystemInfoProbe::capabilities() const
{
    return {.hasOs = true, .unavailableReason = {}};
}

OsInfo WindowsSystemInfoProbe::readOs()
{
    OsInfo info;
    info.family = OsFamily::Windows;

    const std::string currentBuild = readVersionString(L"CurrentBuild");
    const std::optional<std::uint32_t> build = WindowsOsInfo::parseBuild(currentBuild);
    info.name = WindowsOsInfo::productName(readVersionString(L"ProductName"), readVersionString(L"EditionID"), build);
    info.version = WindowsOsInfo::displayVersion(readVersionString(L"DisplayVersion"), readVersionString(L"ReleaseId"));
    info.build = WindowsOsInfo::buildString(currentBuild, readVersionDword(L"UBR"));
    info.installUnixSeconds = readVersionDword(L"InstallDate").value_or(0); // Unix seconds

    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) != FALSE)
    {
        info.architecture = std::string(WindowsOsInfo::machineName(nativeMachine));
    }

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto upSeconds = static_cast<std::int64_t>(GetTickCount64() / 1000);
    info.bootUnixSeconds = now > upSeconds ? static_cast<std::uint64_t>(now - upSeconds) : 0;

    info.computerName =
        readSizedString([](wchar_t* buffer, ULONG& size) { return GetComputerNameExW(ComputerNameDnsHostname, buffer, &size); });
    readJoinInformation(info);
    info.userName = readSizedString([](wchar_t* buffer, ULONG& size) { return GetUserNameExW(NameSamCompatible, buffer, &size); });

    std::wstring locale(LOCALE_NAME_MAX_LENGTH, L'\0');
    if (const int length = GetUserDefaultLocaleName(locale.data(), LOCALE_NAME_MAX_LENGTH); length > 1)
    {
        locale.resize(static_cast<std::size_t>(length) - 1);
        info.locale = WinString::wideToUtf8(locale);
    }
    readTimeZone(info);

    info.systemDirectory = readDirectory([](wchar_t* buffer, UINT size) { return GetSystemDirectoryW(buffer, size); });
    info.windowsDirectory = readDirectory([](wchar_t* buffer, UINT size) { return GetSystemWindowsDirectoryW(buffer, size); });
    return info;
}

FirmwareInfo WindowsSystemInfoProbe::readFirmware()
{
    FirmwareInfo info;
    info.available = true;

    FIRMWARE_TYPE type = FirmwareTypeUnknown;
    if (GetFirmwareType(&type) != FALSE)
    {
        if (type == FirmwareTypeUefi)
        {
            info.firmwareMode = FirmwareMode::Uefi;
        }
        else if (type == FirmwareTypeBios)
        {
            info.firmwareMode = FirmwareMode::Legacy;
        }
    }

    // 'RSMB': the raw SMBIOS table; no administrator rights needed. The size can change between the two
    // calls (it doesn't in practice), so a second call that wants more room leaves the facts empty.
    constexpr DWORD RSMB = 0x52534D42;
    if (const UINT size = GetSystemFirmwareTable(RSMB, 0, nullptr, 0); size > 0)
    {
        std::vector<std::uint8_t> table(size);
        const UINT written = GetSystemFirmwareTable(RSMB, 0, table.data(), size);
        if (written > 0 && written <= size)
        {
            table.resize(written);
            Smbios::decodeFirmware(table, info);
        }
    }

    // The power manager's role (from the ACPI FADT preferred profile) is what msinfo32 shows; the
    // chassis-derived role from the SMBIOS table stands in when it is unspecified.
    if (const std::string_view role =
            WindowsOsInfo::platformRoleName(static_cast<std::uint32_t>(PowerDeterminePlatformRoleEx(POWER_PLATFORM_ROLE_V2)));
        !role.empty())
    {
        info.platformRole = std::string(role);
    }
    return info;
}

} // namespace Platform
