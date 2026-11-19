#include "WindowsProcessModules.h"

#include "Platform/IProcessActions.h"
#include "Platform/IProcessModules.h"
#include "WinString.h"
#include "WindowsDevicePaths.h"
#include "WindowsHandles.h"
#include "WindowsHeldFileVersion.h"
#include "WindowsNtQuery.h"
#include "WindowsProcessIdentity.h"
#include "WindowsVersionResourceMath.h"

// clang-format off
#include <winioctl.h> // FILE_DEVICE_DISK, FILE_DEVICE_CD_ROM, FILE_DEVICE_VIRTUAL_DISK
#include <winternl.h> // NtCreateFile's OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and UNICODE_STRING
// clang-format on

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

/// OBJ_DONT_REPARSE (ntdef.h, which user-mode code doesn't include): the open fails, rather than following any reparse
/// it meets on the way (a junction, a symlink, an object manager link), with STATUS_REPARSE_POINT_ENCOUNTERED.
#ifdef OBJ_DONT_REPARSE
constexpr ULONG DONT_REPARSE = OBJ_DONT_REPARSE;
#else
constexpr ULONG DONT_REPARSE = 0x00001000UL;
#endif

/// FileFsDeviceInformation, the FS_INFORMATION_CLASS NtQueryVolumeInformationFile is asked for, and what it fills
/// (FILE_FS_DEVICE_INFORMATION, ntddk.h), and its FILE_REMOTE_DEVICE characteristic.
constexpr ULONG FILE_FS_DEVICE_INFORMATION_CLASS = 4;
struct FileFsDeviceInformation
{
    DWORD DeviceType = 0;
    ULONG Characteristics = 0;
};
constexpr ULONG REMOTE_DEVICE = 0x00000010UL;

/// ntdll's NtCreateFile, found once; null when it can't be.
[[nodiscard]] LocalFileFunctions::NtCreateFileFn ntdllNtCreateFile() noexcept
{
    static const auto fn = ntdllProc<LocalFileFunctions::NtCreateFileFn>("NtCreateFile");
    return fn;
}

/// ntdll's NtQueryVolumeInformationFile, found once; null when it can't be.
[[nodiscard]] LocalFileFunctions::NtQueryVolumeInformationFileFn ntdllNtQueryVolumeInformationFile() noexcept
{
    static const auto fn = ntdllProc<LocalFileFunctions::NtQueryVolumeInformationFileFn>("NtQueryVolumeInformationFile");
    return fn;
}

/// The version of the file @p file is open on, as heldFileVersion() reads it, as its four parts.
[[nodiscard]] std::optional<ModuleVersion> heldModuleVersion(HANDLE file)
{
    const std::optional<VersionResourceMath::FixedFileVersion> fixed = heldFileVersion(file);
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

/// Whether the held @p file may be read for its version (#4373): not a name-surrogate reparse point (a symlink or a
/// junction, which FILE_OPEN_REPARSE_POINT opened as itself), on a disk, a virtual disk or a CD-ROM that isn't a remote
/// device, and not a file a redirector serves. Each is a backstop for the read: the open itself is what keeps a
/// connection from being made. A reparse point that isn't a name surrogate (a compressed system file's WOF tag, a
/// deduplicated or cloud file) is the file itself, so it is read.
[[nodiscard]] bool mayRead(HANDLE file, const LocalFileFunctions& fns)
{
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (fns.getFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) == 0 ||
        ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(tag.ReparseTag)))
    {
        return false;
    }
    IO_STATUS_BLOCK status{};
    FileFsDeviceInformation device{};
    if (fns.ntQueryVolumeInformationFile(file, &status, &device, sizeof(device), FILE_FS_DEVICE_INFORMATION_CLASS) < 0 ||
        (device.DeviceType != FILE_DEVICE_DISK && device.DeviceType != FILE_DEVICE_VIRTUAL_DISK &&
         device.DeviceType != FILE_DEVICE_CD_ROM) ||
        (device.Characteristics & REMOTE_DEVICE) != 0)
    {
        return false;
    }
    // Only a redirector answers this, for its own files.
    FILE_REMOTE_PROTOCOL_INFO remote{};
    return fns.getFileInformationByHandleEx(file, FileRemoteProtocolInfo, &remote, sizeof(remote)) == 0;
}

} // namespace

LocalFileFunctions systemLocalFileFunctions() noexcept
{
    return {
        .ntCreateFile = ntdllNtCreateFile(),
        .ntQueryVolumeInformationFile = ntdllNtQueryVolumeInformationFile(),
        .getFileInformationByHandleEx = &::GetFileInformationByHandleEx,
        .readHeldVersion = &heldModuleVersion,
    };
}

std::optional<ModuleVersion> readLocalFileVersion(const std::wstring& ntName, const LocalFileFunctions& fns)
{
    if (fns.ntCreateFile == nullptr || fns.ntQueryVolumeInformationFile == nullptr || fns.getFileInformationByHandleEx == nullptr ||
        fns.readHeldVersion == nullptr || ntName.empty() || ntName.size() * sizeof(wchar_t) > 0xFFFEU)
    {
        return std::nullopt;
    }
    // The kernel's NT name itself, never through \\?\GLOBALROOT (a symbolic link) or a drive letter. OBJ_DONT_REPARSE: a
    // junction or symlink anywhere on the way fails the open instead of being followed, so a link the process put there
    // since its module was named can't send the open to a share; the kernel's name is the one left after every reparse,
    // so a real module never needs one. FILE_OPEN_REPARSE_POINT: a link at the name itself is opened as itself.
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(ntName.c_str()); // NOLINT(cppcoreguidelines-pro-type-const-cast) - NtCreateFile only reads it
    name.Length = static_cast<USHORT>(ntName.size() * sizeof(wchar_t));
    name.MaximumLength = name.Length;
    // Identification only: whatever answers the open (a pipe server, should one be reached) can learn who is asking but
    // can't act as TaskSmack, or as the elevated helper.
    SECURITY_QUALITY_OF_SERVICE qos{};
    qos.Length = sizeof(qos);
    qos.ImpersonationLevel = SecurityIdentification;
    qos.ContextTrackingMode = SECURITY_STATIC_TRACKING;
    qos.EffectiveOnly = TRUE;
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    attributes.ObjectName = &name;
    attributes.Attributes = OBJ_CASE_INSENSITIVE | DONT_REPARSE;
    attributes.SecurityQualityOfService = &qos;
    IO_STATUS_BLOCK status{};
    HANDLE opened = nullptr;
    // Shared for read and delete (the loader's own opens share delete), never for write, so the bytes stay put.
    const NTSTATUS result = fns.ntCreateFile(&opened,
                                             GENERIC_READ | SYNCHRONIZE,
                                             &attributes,
                                             &status,
                                             nullptr,
                                             FILE_ATTRIBUTE_NORMAL,
                                             FILE_SHARE_READ | FILE_SHARE_DELETE,
                                             FILE_OPEN,
                                             FILE_OPEN_REPARSE_POINT | FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
                                             nullptr,
                                             0);
    if (result < 0)
    {
        return std::nullopt;
    }
    const UniqueHandle file(opened);
    if (!file || !mayRead(file.get(), fns))
    {
        return std::nullopt;
    }
    return fns.readHeldVersion(file.get());
}

std::optional<ModuleVersion> readLocalFileVersion(const std::wstring& ntName)
{
    return readLocalFileVersion(ntName, systemLocalFileFunctions());
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
