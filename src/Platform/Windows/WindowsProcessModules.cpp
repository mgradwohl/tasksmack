#include "WindowsProcessModules.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "WinString.h"
#include "WindowsDevicePaths.h"
#include "WindowsHandles.h"
#include "WindowsHeldFileVersion.h"
#include "WindowsProcessIdentity.h"
#include "WindowsVersionResourceMath.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace Platform::Windows
{

namespace
{

/// The most file versions kept: past it the cache starts over, so browsing many processes cannot grow
/// it without bound. A desktop's distinct module files number in the low thousands.
constexpr std::size_t MAX_CACHED_VERSIONS = 4096;

/// The longest path GetModuleFileNameExW or GetMappedFileNameW can return (an extended-length path), in characters.
constexpr DWORD MAX_MODULE_PATH = 32768;

/// Tries at listing the modules before taking what fits: the list grows as the process loads more.
constexpr int MAX_ENUM_ATTEMPTS = 3;

[[nodiscard]] ModulesReadResult failure(ModulesReadStatus status, std::string detail = {})
{
    return {.status = status, .modules = {}, .detail = std::move(detail)};
}

/// The system's message for a Win32 error code ("Only part of a ReadProcessMemory ... was completed").
[[nodiscard]] std::string errorText(DWORD error)
{
    return std::system_category().message(static_cast<int>(error));
}

/// The NT devices of this run's local drive letters: fixed, removable, optical and RAM disks, never network drives
/// (#4373). The drive letters are TaskSmack's user's own (DOS devices are per sign-in), never the target process's.
[[nodiscard]] std::vector<std::wstring> localDriveDevices(const ProcessModuleFunctions& api)
{
    std::vector<std::wstring> devices;
    for (WindowsDevicePaths::DriveDevice& drive : WindowsDevicePaths::driveDevices(api.getLogicalDrives, api.queryDosDevice))
    {
        const std::array<wchar_t, 4> root{drive.letter, L':', L'\\', L'\0'};
        const UINT type = api.getDriveType(root.data());
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK)
        {
            devices.push_back(std::move(drive.device));
        }
    }
    return devices;
}

/// Whether @p ntName names a file on @p device ("\Device\HarddiskVolume3\x.dll" on "\Device\HarddiskVolume3").
[[nodiscard]] bool isOnDevice(std::wstring_view ntName, std::wstring_view device) noexcept
{
    return !device.empty() && ntName.size() > device.size() + 1 && ntName.starts_with(device) && ntName[device.size()] == L'\\';
}

/// Whether the NT name @p ntName (GetMappedFileNameW's) is a file on a local volume, so it may be opened (#4373): on a
/// volume the volume manager names ("\Device\HarddiskVolumeN\...", with a drive letter or without), or on the device
/// of one of @p localDevices. A network share's file (under "\Device\Mup\", or a redirector's device), a named pipe,
/// or any other device's is not. A name with a ".." component, which none the kernel gives has, is refused too.
[[nodiscard]] bool isLocalFile(std::wstring_view ntName, std::span<const std::wstring> localDevices) noexcept
{
    if (ntName.contains(L"\\..\\") || ntName.ends_with(L"\\.."))
    {
        return false;
    }
    constexpr std::wstring_view VOLUME = L"\\Device\\HarddiskVolume";
    if (ntName.starts_with(VOLUME))
    {
        std::size_t at = VOLUME.size();
        while (at < ntName.size() && std::iswdigit(ntName[at]) != 0)
        {
            ++at;
        }
        if (at > VOLUME.size() && at + 1 < ntName.size() && ntName[at] == L'\\')
        {
            return true;
        }
    }
    return std::ranges::any_of(localDevices, [ntName](const std::wstring& device) { return isOnDevice(ntName, device); });
}

} // namespace

std::optional<ModuleVersion> readLocalFileVersion(const std::wstring& ntName)
{
    // The NT name as the kernel gave it, through \\?\GLOBALROOT, so no drive letter or DOS device reinterprets it.
    // Shared for read and delete (the loader's own opens share delete), never for write, so the bytes stay put.
    const std::wstring path = L"\\\\?\\GLOBALROOT" + ntName;
    const UniqueHandle file(
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file)
    {
        return std::nullopt;
    }
    // Only a redirector answers this, for its own files: a name found local that led to one after all isn't read.
    FILE_REMOTE_PROTOCOL_INFO remote{};
    if (GetFileInformationByHandleEx(file.get(), FileRemoteProtocolInfo, &remote, sizeof(remote)) != 0)
    {
        return std::nullopt;
    }
    const std::optional<VersionResourceMath::FixedFileVersion> fixed = heldFileVersion(file.get());
    if (!fixed.has_value())
    {
        return std::nullopt;
    }
    return ModuleVersion{
        .major = HIWORD(fixed->mostSignificant),
        .minor = LOWORD(fixed->mostSignificant),
        .build = HIWORD(fixed->leastSignificant),
        .revision = LOWORD(fixed->leastSignificant),
    };
}

bool WindowsProcessModulesReader::hasModules() const
{
    return true;
}

ModulesReadResult WindowsProcessModulesReader::readModules(const ProcessTarget& target)
{
    // PROCESS_VM_READ: the module list lives in the process's own memory (its PEB loader data).
    // Protected processes, and elevated ones from an unelevated TaskSmack, refuse it.
    auto opened = openProcessVerified(
        {
            .openProcess = m_Api.openProcess,
            .getProcessTimes = m_Api.getProcessTimes,
            .closeHandle = m_Api.closeHandle,
            .getExitCodeProcess = m_Api.getExitCodeProcess,
        },
        target,
        PROCESS_VM_READ);
    if (!opened)
    {
        auto refused = toReadFailure<ModulesReadStatus>(opened.error());
        return failure(refused.status, std::move(refused.detail));
    }
    const InjectableHandle process = std::move(*opened);

    std::vector<HMODULE> handles(256);
    for (int attempt = 1;; ++attempt)
    {
        const auto bytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
        DWORD needed = 0;
        if (m_Api.enumProcessModulesEx(process.get(), handles.data(), bytes, &needed, LIST_MODULES_ALL) == 0)
        {
            const DWORD error = GetLastError();
            // Exited since openProcessVerified() checked: reported as it would have been there.
            DWORD exitCode = 0;
            if (m_Api.getExitCodeProcess(process.get(), &exitCode) != 0 && exitCode != STILL_ACTIVE)
            {
                return failure(ModulesReadStatus::ProcessExited);
            }
            if (error == ERROR_ACCESS_DENIED)
            {
                return failure(ModulesReadStatus::PermissionDenied);
            }
            // ERROR_PARTIAL_COPY: a process still starting has no loader data yet; retried at the next refresh.
            return failure(ModulesReadStatus::Failed, errorText(error));
        }
        if (needed <= bytes || attempt == MAX_ENUM_ATTEMPTS)
        {
            handles.resize(std::min(needed, bytes) / sizeof(HMODULE));
            break;
        }
        handles.resize((needed / sizeof(HMODULE)) + 32); // room for a few more loaded in between
    }

    ModulesReadResult result{.status = ModulesReadStatus::Ok, .modules = {}, .detail = {}};
    result.modules.reserve(handles.size());
    std::wstring buffer(MAX_MODULE_PATH, L'\0');
    std::wstring mapped(MAX_MODULE_PATH, L'\0');
    std::optional<std::vector<std::wstring>> localDevices; // read on the first module that needs them
    for (HMODULE module : handles)
    {
        // The loader data's FullDllName: the process's own memory, which it can rewrite to any path, so it is shown
        // and never opened (#4373).
        const DWORD length = m_Api.getModuleFileNameEx(process.get(), module, buffer.data(), MAX_MODULE_PATH);
        MODULEINFO info{};
        if (length == 0 || m_Api.getModuleInformation(process.get(), module, &info, sizeof(info)) == 0)
        {
            continue; // unloaded since the listing
        }
        ProcessModule row{
            .path = WinString::wideToUtf8(std::wstring_view(buffer.data(), length)),
            .baseAddress = reinterpret_cast<std::uintptr_t>(info.lpBaseOfDll),
            .sizeBytes = info.SizeOfImage,
            .version = std::nullopt,
            .deleted = false,
            .versionNotRead = false,
        };
        // The file the image section was made from, as the kernel names it: the one the version is read from. A
        // module that can't be named (unloaded since, or a name too long) keeps its row, with no version.
        const DWORD mappedLength = m_Api.getMappedFileName(process.get(), info.lpBaseOfDll, mapped.data(), MAX_MODULE_PATH);
        if (mappedLength != 0 && mappedLength < MAX_MODULE_PATH - 1)
        {
            const std::wstring ntName(mapped.data(), mappedLength);
            if (!localDevices.has_value())
            {
                localDevices = localDriveDevices(m_Api);
            }
            if (isLocalFile(ntName, *localDevices))
            {
                row.version = fileVersion(ntName);
            }
            else
            {
                row.versionNotRead = true; // a network share's or a device's: never opened
            }
        }
        result.modules.push_back(std::move(row));
    }
    return result;
}

std::optional<ModuleVersion> WindowsProcessModulesReader::fileVersion(const std::wstring& ntName)
{
    if (const auto cached = m_VersionCache.find(ntName); cached != m_VersionCache.end())
    {
        return cached->second;
    }
    const std::optional<ModuleVersion> version = m_Api.readFileVersion(ntName);
    if (m_VersionCache.size() >= MAX_CACHED_VERSIONS)
    {
        m_VersionCache.clear();
    }
    m_VersionCache.emplace(ntName, version);
    return version;
}

} // namespace Platform::Windows
