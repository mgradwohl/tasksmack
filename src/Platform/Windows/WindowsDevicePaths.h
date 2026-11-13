#pragma once

// NT device paths as the DOS paths a user knows (#1516, #3089): "\Device\HarddiskVolume3\Windows\x.dll" ->
// "C:\Windows\x.dll" through each drive letter's NT device (QueryDosDeviceW), "\Device\Mup\server\share\x"
// -> "\\server\share\x", and the "\??\" and "\\?\" forms stripped. The page file list (WindowsCommitPaging.h)
// and the memory map's mapped files (WindowsProcessMemoryMap.h, from GetMappedFileNameW) share it. Pure but
// for driveDevices(), whose two calls are passed in so tests can fake them.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::WindowsDevicePaths
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// A drive letter and the NT device it names ('C', "\Device\HarddiskVolume3"), from QueryDosDeviceW.
struct DriveDevice
{
    wchar_t letter = L'\0';
    std::wstring device;
};

/// An NT path as a DOS path: "\??\C:\pagefile.sys" -> "C:\pagefile.sys", "\??\UNC\server\share\x" ->
/// "\\server\share\x", "\Device\Mup\server\share\x" -> "\\server\share\x", and
/// "\Device\HarddiskVolume3\pagefile.sys" -> "C:\pagefile.sys" through @p drives. A path none of those fit
/// (a volume with no drive letter) is returned as given.
[[nodiscard]] inline std::wstring ntPathToDosPath(std::wstring_view ntPath, std::span<const DriveDevice> drives)
{
    for (const std::wstring_view prefix :
         {std::wstring_view{L"\\??\\"}, std::wstring_view{L"\\\\?\\"}, std::wstring_view{L"\\DosDevices\\"}})
    {
        if (ntPath.starts_with(prefix))
        {
            const std::wstring_view rest = ntPath.substr(prefix.size());
            if (rest.starts_with(L"UNC\\"))
            {
                return L"\\\\" + std::wstring(rest.substr(4));
            }
            return std::wstring(rest);
        }
    }
    // The multiple UNC provider: every network file a process maps is named under it.
    if (constexpr std::wstring_view MUP = L"\\Device\\Mup\\"; ntPath.starts_with(MUP))
    {
        return L"\\\\" + std::wstring(ntPath.substr(MUP.size()));
    }
    for (const DriveDevice& drive : drives)
    {
        if (!drive.device.empty() && ntPath.starts_with(drive.device) &&
            (ntPath.size() == drive.device.size() || ntPath[drive.device.size()] == L'\\'))
        {
            return std::wstring{drive.letter, L':'} + std::wstring(ntPath.substr(drive.device.size()));
        }
    }
    return std::wstring(ntPath);
}

/// Every drive letter's NT device, for ntPathToDosPath(), through @p getLogicalDrives and @p queryDosDevice
/// (GetLogicalDrives and QueryDosDeviceW, or a test's fakes). A letter whose device can't be read is left out.
[[nodiscard]] inline std::vector<DriveDevice> driveDevices(decltype(&GetLogicalDrives) getLogicalDrives,
                                                           decltype(&QueryDosDeviceW) queryDosDevice)
{
    std::vector<DriveDevice> drives;
    const DWORD mask = getLogicalDrives();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter)
    {
        if ((mask & (1U << static_cast<unsigned>(letter - L'A'))) == 0)
        {
            continue;
        }
        const std::array<wchar_t, 3> name{letter, L':', L'\0'};
        std::array<wchar_t, MAX_PATH> target{};
        if (queryDosDevice(name.data(), target.data(), static_cast<DWORD>(target.size())) != 0)
        {
            drives.push_back({.letter = letter, .device = std::wstring(target.data())}); // the first of the NUL-separated targets
        }
    }
    return drives;
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::WindowsDevicePaths

#endif // _WIN32
