#pragma once

// A local file opened by the kernel's NT name for it, with nothing on the way followed (#4373, #4482): the one open the
// Modules section's version read (readLocalFileVersion()) and the image file the Publisher column and Look up on
// VirusTotal read (openProcessImageFile()) both make. The name comes from the kernel (GetMappedFileNameW), but the
// process it names can change the path under it between that read and the open, swapping the file, or a folder above
// it, for a symbolic link or a junction to \\host\share. Opened by name in the usual way (CreateFileW, flags 0), that
// open would follow the link and connect to the host, as TaskSmack's user or the elevated helper, before anything
// checked the handle.
//
// So the open is NtCreateFile on the NT name itself, never through \\?\GLOBALROOT (itself a symbolic link) or a drive
// letter, with OBJ_DONT_REPARSE (a junction or symlink met on the way fails the open, STATUS_REPARSE_POINT_ENCOUNTERED,
// rather than being followed; the kernel's name is the one left after every reparse, so a real file never needs one)
// and FILE_OPEN_REPARSE_POINT (a link at the name itself is opened as itself, not followed). It asks for
// identification-level security (SECURITY_QUALITY_OF_SERVICE), so whatever answers it, a pipe server should one be
// reached, can learn who is asking but can't act as them. A name under \Device\Mup\ (a network share's: every UNC open
// goes through the multiple UNC provider), under an in-box redirector's device (\Device\LanmanRedirector\,
// \Device\WebDavRedirector\) or under \Device\NamedPipe\ is refused before any open, since opening it is the connection.
//
// After the open, a handle that is a name-surrogate reparse point (a link, opened as itself), on a device that isn't a
// disk, a virtual disk or a CD-ROM, on a remote device, or that a redirector serves, is closed and refused: a backstop
// for whatever reads the file, not for the connection, which the open itself guards. A reparse point that isn't a
// name surrogate (a CompactOS file's WOF tag, a deduplicated or cloud file) is the file itself, so it is kept.
//
// Its own file, so TaskSmackHelper.exe, which reads modules for an administrator (#2309), builds it in without the
// image file code (TASKSMACK_WINDOWS_HELPER_SOURCES).

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
#include <winternl.h> // NtCreateFile's OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and UNICODE_STRING
// clang-format on

#include "WindowsHandles.h"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// The calls openLocalNtFile() makes: ntdll's and kernel32's (systemLocalFileFunctions()), or a test's fakes.
struct LocalFileFunctions
{
    using NtCreateFileFn = NTSTATUS(NTAPI*)(
        PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
    /// NtQueryVolumeInformationFile, its FS_INFORMATION_CLASS passed as a ULONG.
    using NtQueryVolumeInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
    using GetFileInformationByHandleExFn = BOOL(WINAPI*)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);

    NtCreateFileFn ntCreateFile = nullptr;
    NtQueryVolumeInformationFileFn ntQueryVolumeInformationFile = nullptr;
    GetFileInformationByHandleExFn getFileInformationByHandleEx = nullptr;
};

/// The system's calls: ntdll's NtCreateFile and NtQueryVolumeInformationFile (found with GetModuleHandleW, which ntdll
/// always answers) and GetFileInformationByHandleEx.
[[nodiscard]] LocalFileFunctions systemLocalFileFunctions() noexcept;

/// What openLocalNtFile() is asked to open the file for.
struct LocalFileAccess
{
    /// The access wanted; SYNCHRONIZE is added, for synchronous I/O on the handle.
    ACCESS_MASK access = GENERIC_READ;
    /// The sharing allowed to other opens (FILE_SHARE_*).
    ULONG share = FILE_SHARE_READ;
    /// FILE_SEQUENTIAL_ONLY, CreateFileW's FILE_FLAG_SEQUENTIAL_SCAN: the file is read once from start to end (a hash).
    bool sequential = false;
};

/// Why openLocalNtFile() gave no handle.
enum class LocalFileRefusal : std::uint8_t
{
    Unavailable, ///< ntdll's calls weren't found, or the name is empty or longer than a UNICODE_STRING holds
    NotLocal,    ///< a network share's name, or a handle on a device that isn't a local disk, or a redirector's
    Link,        ///< the name is a symbolic link or a junction itself (opened as itself, and closed)
    OpenFailed,  ///< NtCreateFile failed: no such file, access denied, or a link on the way (never followed)
};

/// openLocalNtFile()'s refusal: why, and for OpenFailed the NTSTATUS and the Win32 error it maps to.
struct LocalFileError
{
    LocalFileRefusal reason = LocalFileRefusal::Unavailable;
    NTSTATUS status = 0;
    DWORD win32Error = 0;
};

/// The file whose NT name is @p ntName ("\Device\HarddiskVolume3\Windows\System32\ntdll.dll", as GetMappedFileNameW
/// names it), opened as the top of this file says for @p access, or why not. A link on the way, a link at the name, a
/// network share's name and a handle off a local disk are all refused, never followed or read.
[[nodiscard]] std::expected<UniqueHandle, LocalFileError>
openLocalNtFile(std::wstring_view ntName, const LocalFileAccess& access, const LocalFileFunctions& fns);

/// openLocalNtFile() through the system's calls.
[[nodiscard]] std::expected<UniqueHandle, LocalFileError> openLocalNtFile(std::wstring_view ntName, const LocalFileAccess& access);

/// @p error in words, to follow "can't be read: " ("The system cannot find the file specified.", "it is a link, which
/// TaskSmack doesn't follow").
[[nodiscard]] std::string describeLocalFileError(const LocalFileError& error);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows

#endif // _WIN32
