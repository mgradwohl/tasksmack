#include "WindowsLocalNtFile.h"

#include "WinString.h"
#include "WindowsHandles.h"
#include "WindowsNtQuery.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <winioctl.h> // FILE_DEVICE_DISK, FILE_DEVICE_CD_ROM, FILE_DEVICE_VIRTUAL_DISK
#include <winternl.h> // NtCreateFile's OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and UNICODE_STRING
// clang-format on

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Windows APIs; Win32 types come from windows.h sub-headers
namespace
{

/// OBJ_DONT_REPARSE (ntdef.h, which user-mode code doesn't include): the open fails, rather than following any reparse
/// it meets on the way (a junction, a symlink, an object manager link), with STATUS_REPARSE_POINT_ENCOUNTERED.
#ifdef OBJ_DONT_REPARSE
constexpr ULONG DONT_REPARSE = OBJ_DONT_REPARSE;
#else
constexpr ULONG DONT_REPARSE = 0x00001000UL;
#endif

/// FILE_SEQUENTIAL_ONLY (winternl.h has it on newer SDKs only).
#ifdef FILE_SEQUENTIAL_ONLY
constexpr ULONG SEQUENTIAL_ONLY = FILE_SEQUENTIAL_ONLY;
#else
constexpr ULONG SEQUENTIAL_ONLY = 0x00000004UL;
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

/// The devices whose names are refused before any open (see the top of the header): opening a name under one is a
/// connection to a share or a pipe server.
constexpr std::array<std::wstring_view, 4> CONNECTING_DEVICES{
    L"\\Device\\Mup\\",
    L"\\Device\\LanmanRedirector\\",
    L"\\Device\\WebDavRedirector\\",
    L"\\Device\\NamedPipe\\",
};

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

/// @p status as a Win32 error (ntdll's RtlNtStatusToDosError); ERROR_GEN_FAILURE when that can't be found.
[[nodiscard]] DWORD win32ErrorOf(NTSTATUS status) noexcept
{
    using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS);
    static const auto fn = ntdllProc<RtlNtStatusToDosErrorFn>("RtlNtStatusToDosError");
    return fn == nullptr ? static_cast<DWORD>(ERROR_GEN_FAILURE) : static_cast<DWORD>(fn(status));
}

/// Whether @p ntName is under one of CONNECTING_DEVICES, case aside.
[[nodiscard]] bool isConnectingName(std::wstring_view ntName) noexcept
{
    return std::ranges::any_of(CONNECTING_DEVICES,
                               [ntName](std::wstring_view device)
                               {
                                   if (ntName.size() < device.size())
                                   {
                                       return false;
                                   }
                                   const std::wstring_view start = ntName.substr(0, device.size());
                                   const auto length = static_cast<int>(device.size());
                                   // Each length is given, so neither needs a terminator.
                                   // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage) - lengths passed
                                   return CompareStringOrdinal(start.data(), length, device.data(), length, TRUE) == CSTR_EQUAL;
                               });
}

/// Why the held @p file isn't kept (see the top of the header), or nothing when it is.
[[nodiscard]] std::expected<void, LocalFileRefusal> checkHeld(HANDLE file, const LocalFileFunctions& fns)
{
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (fns.getFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag)) == 0)
    {
        return std::unexpected(LocalFileRefusal::NotLocal);
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(tag.ReparseTag))
    {
        return std::unexpected(LocalFileRefusal::Link);
    }
    IO_STATUS_BLOCK status{};
    FileFsDeviceInformation device{};
    if (fns.ntQueryVolumeInformationFile(file, &status, &device, sizeof(device), FILE_FS_DEVICE_INFORMATION_CLASS) < 0 ||
        (device.DeviceType != FILE_DEVICE_DISK && device.DeviceType != FILE_DEVICE_VIRTUAL_DISK &&
         device.DeviceType != FILE_DEVICE_CD_ROM) ||
        (device.Characteristics & REMOTE_DEVICE) != 0)
    {
        return std::unexpected(LocalFileRefusal::NotLocal);
    }
    // Only a redirector answers this, for its own files.
    FILE_REMOTE_PROTOCOL_INFO remote{};
    if (fns.getFileInformationByHandleEx(file, FileRemoteProtocolInfo, &remote, sizeof(remote)) != 0)
    {
        return std::unexpected(LocalFileRefusal::NotLocal);
    }
    return {};
}

} // namespace

LocalFileFunctions systemLocalFileFunctions() noexcept
{
    return {
        .ntCreateFile = ntdllNtCreateFile(),
        .ntQueryVolumeInformationFile = ntdllNtQueryVolumeInformationFile(),
        .getFileInformationByHandleEx = &::GetFileInformationByHandleEx,
    };
}

std::expected<UniqueHandle, LocalFileError>
openLocalNtFile(std::wstring_view ntName, const LocalFileAccess& access, const LocalFileFunctions& fns)
{
    if (fns.ntCreateFile == nullptr || fns.ntQueryVolumeInformationFile == nullptr || fns.getFileInformationByHandleEx == nullptr ||
        ntName.empty() || ntName.size() * sizeof(wchar_t) > 0xFFFEU)
    {
        return std::unexpected(LocalFileError{.reason = LocalFileRefusal::Unavailable});
    }
    if (isConnectingName(ntName))
    {
        return std::unexpected(LocalFileError{.reason = LocalFileRefusal::NotLocal});
    }
    // The kernel's NT name itself, never through \\?\GLOBALROOT (a symbolic link) or a drive letter. OBJ_DONT_REPARSE: a
    // junction or symlink anywhere on the way fails the open instead of being followed, so a link the process put there
    // since the name was read can't send the open to a share. FILE_OPEN_REPARSE_POINT: a link at the name itself is
    // opened as itself.
    UNICODE_STRING name{};
    name.Buffer = const_cast<PWSTR>(ntName.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast) - NtCreateFile only reads it
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
    const ULONG options =
        FILE_OPEN_REPARSE_POINT | FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | (access.sequential ? SEQUENTIAL_ONLY : 0UL);
    const NTSTATUS result = fns.ntCreateFile(&opened,
                                             access.access | SYNCHRONIZE,
                                             &attributes,
                                             &status,
                                             nullptr,
                                             FILE_ATTRIBUTE_NORMAL,
                                             access.share,
                                             FILE_OPEN,
                                             options,
                                             nullptr,
                                             0);
    if (result < 0)
    {
        return std::unexpected(
            LocalFileError{.reason = LocalFileRefusal::OpenFailed, .status = result, .win32Error = win32ErrorOf(result)});
    }
    UniqueHandle file(opened);
    if (!file)
    {
        return std::unexpected(LocalFileError{.reason = LocalFileRefusal::Unavailable});
    }
    if (const std::expected<void, LocalFileRefusal> kept = checkHeld(file.get(), fns); !kept.has_value())
    {
        return std::unexpected(LocalFileError{.reason = kept.error()});
    }
    return file;
}

std::expected<UniqueHandle, LocalFileError> openLocalNtFile(std::wstring_view ntName, const LocalFileAccess& access)
{
    return openLocalNtFile(ntName, access, systemLocalFileFunctions());
}

std::string describeLocalFileError(const LocalFileError& error)
{
    switch (error.reason)
    {
    case LocalFileRefusal::OpenFailed:
        return WinString::win32ErrorText(error.win32Error);
    case LocalFileRefusal::Link:
        return "it is a symbolic link or a junction, which TaskSmack doesn't follow";
    case LocalFileRefusal::NotLocal:
        return "it isn't on a drive TaskSmack can check";
    case LocalFileRefusal::Unavailable:
        break;
    }
    return "Windows' calls to open it aren't available";
}

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
