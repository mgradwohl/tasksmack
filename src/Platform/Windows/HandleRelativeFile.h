#pragma once

// Files and folders opened relative to a folder handle held open, never by re-walking a path (#4143): the NT calls
// PinnedFolder (the handed-over config folder, #3774) and App::DebugLogFile (an elevated session's log, #4105) share,
// so the security-critical handle code has one copy, in Platform.
//   - openRelative(): NtCreateFile with OBJECT_ATTRIBUTES.RootDirectory, one plain name under the held parent. It always
//     opens a link, junction or mount point itself (FILE_OPEN_REPARSE_POINT), never what it names.
//   - renameRelative() and renameAside(): NtSetInformationFile's rename with RootDirectory, which kernel32's
//     SetFileInformationByHandle refuses.
//   - deleteThroughHandle() and markDeleteOnClose(): an entry deleted through the handle that holds it, never by name.
//   - secureChildFolder(): a folder under a held parent, made fresh with a security descriptor, or taken over in place
//     when an administrator owns it, or moved aside when it can't be trusted.
// The NT calls are function pointers, defaulting to ntdll's (resolved with GetModuleHandleW, which ntdll always
// answers), so a test can drive each answer.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h> // NtCreateFile's OBJECT_ATTRIBUTES, IO_STATUS_BLOCK and UNICODE_STRING
// clang-format on

#include "Platform/Windows/WindowsHandles.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Platform::Windows
{

using NtCreateFileFn =
    NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
/// Its FILE_INFORMATION_CLASS as the ULONG the enum is passed as.
using NtSetInformationFileFn = NTSTATUS(NTAPI*)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
using GetFinalPathFn = DWORD(WINAPI*)(HANDLE, LPWSTR, DWORD, DWORD);

/// ntdll's NtCreateFile, found when first called; NT_NO_ENTRY_POINT when it can't be.
NTSTATUS NTAPI systemNtCreateFile(PHANDLE handle,
                                  ACCESS_MASK access,
                                  POBJECT_ATTRIBUTES attributes,
                                  PIO_STATUS_BLOCK status,
                                  PLARGE_INTEGER allocationSize,
                                  ULONG fileAttributes,
                                  ULONG share,
                                  ULONG disposition,
                                  ULONG options,
                                  PVOID eaBuffer,
                                  ULONG eaLength);

/// ntdll's NtSetInformationFile, found when first called; NT_NO_ENTRY_POINT when it can't be.
NTSTATUS NTAPI systemNtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK status, PVOID information, ULONG length, ULONG what);

/// The NTSTATUS values these calls answer that callers act on (ntstatus.h, which user-mode code doesn't include).
inline constexpr auto NT_OBJECT_NAME_NOT_FOUND = static_cast<NTSTATUS>(0xC0000034L);
inline constexpr auto NT_OBJECT_NAME_COLLISION = static_cast<NTSTATUS>(0xC0000035L);
inline constexpr auto NT_OBJECT_PATH_NOT_FOUND = static_cast<NTSTATUS>(0xC000003AL);
inline constexpr auto NT_NO_ENTRY_POINT = static_cast<NTSTATUS>(0xC000007AL);

/// Whether @p status says the name, or a folder on the way to it, isn't there.
[[nodiscard]] constexpr bool isNotFound(NTSTATUS status) noexcept
{
    return status == NT_OBJECT_NAME_NOT_FOUND || status == NT_OBJECT_PATH_NOT_FOUND;
}

/// "NTSTATUS 0x<hex>", for an error message.
[[nodiscard]] std::string ntStatusText(NTSTATUS status);
/// GetLastError()'s message, for an error message.
[[nodiscard]] std::string lastErrorText();

/// What an open expects the name to be: a folder, a file, or either (whatever is there, e.g. an entry to be removed).
enum class EntryKind : std::uint8_t
{
    Directory,
    File,
    Either,
};

/// How openRelative() opens a name.
struct RelativeOpen
{
    /// Exactly the access asked, nothing added, so a DELETE-only open stays grantable through the parent's
    /// FILE_DELETE_CHILD. A handle is synchronous (FILE_SYNCHRONOUS_IO_NONALERT) only when SYNCHRONIZE is asked.
    ACCESS_MASK access = 0;
    /// Shared read and write, never delete by default, so it can't be renamed or deleted from under the handle.
    ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    ULONG disposition = FILE_OPEN; ///< FILE_OPEN, FILE_CREATE or FILE_OPEN_IF
    EntryKind kind = EntryKind::Either;
    PSECURITY_DESCRIPTOR security = nullptr; ///< Applied when the open creates it
};

/// What openRelative() opened.
struct RelativeOpened
{
    UniqueHandle handle;
    bool created = false; ///< Whether the open made a new object (IO_STATUS_BLOCK.Information is FILE_CREATED)
};

/// @p name, one plain path component, opened by NtCreateFile relative to the folder @p parent (RootDirectory), case
/// insensitive, as @p how says: a link, junction or mount point is opened itself, never followed. The NTSTATUS when it
/// fails, which isNotFound() tells apart for a name that isn't there.
[[nodiscard]] std::expected<RelativeOpened, NTSTATUS>
openRelative(HANDLE parent, std::wstring_view name, const RelativeOpen& how, NtCreateFileFn ntCreateFile = &systemNtCreateFile);

/// Renames the entry @p file holds (opened with DELETE) to @p newName in the folder @p rootDirectory, through that
/// handle: FILE_RENAME_INFORMATION (@p informationClass 10, FileRenameInformation, whose first ULONG is ReplaceIfExists)
/// or FILE_RENAME_INFORMATION_EX (65, FileRenameInformationEx, whose first ULONG is its flags), @p flags in that first
/// ULONG. The NTSTATUS answered.
[[nodiscard]] NTSTATUS renameRelative(HANDLE file,
                                      HANDLE rootDirectory,
                                      std::wstring_view newName,
                                      ULONG informationClass,
                                      ULONG flags,
                                      NtSetInformationFileFn ntSetInformationFile = &systemNtSetInformationFile);

/// FileRenameInformation and FileRenameInformationEx, which winternl.h's FILE_INFORMATION_CLASS doesn't name.
inline constexpr ULONG FILE_RENAME_INFORMATION_CLASS = 10;
inline constexpr ULONG FILE_RENAME_INFORMATION_EX_CLASS = 65;

/// Renames the entry @p file holds (opened with DELETE) aside, out of the way, in its folder @p parent: a fresh one can
/// then be made under @p name. Any entry can be moved this way, a junction or a planted file included, which only takes
/// it out of the path. The moved-aside name is @p name ".untrusted-" and a random suffix, tried again only when that
/// name is taken, so one left from a crashed run doesn't block it. The NTSTATUS when it can't be moved.
[[nodiscard]] std::expected<void, NTSTATUS>
renameAside(HANDLE file, HANDLE parent, std::wstring_view name, NtSetInformationFileFn ntSetInformationFile = &systemNtSetInformationFile);

/// Marks the entry @p file holds (opened with DELETE) for deletion, through that handle: POSIX semantics where the
/// volume takes them, so the name is free at once, else the classic delete-on-close. False when neither takes.
[[nodiscard]] bool deleteThroughHandle(HANDLE file, NtSetInformationFileFn ntSetInformationFile = &systemNtSetInformationFile);

/// Marks @p file, opened with DELETE, to be deleted when its handle closes (the classic FileDispositionInfo).
void markDeleteOnClose(HANDLE file) noexcept;

/// GetFinalPathNameByHandleW's path for @p handle as a plain drive path, without the \\?\ prefix (a \\?\UNC\ path
/// keeps it); nullopt when it can't be read. WindowsFinalPath.h's readFinalPath(), sized FinalPathBuffer::AsAnswered,
/// FinalPathPrefix::KeptForUnc.
[[nodiscard]] std::optional<std::wstring> finalPathOf(HANDLE handle, GetFinalPathFn getFinalPath = &::GetFinalPathNameByHandleW);

/// Whether @p handle is a plain folder (not a reparse point), read through it; nullopt when it can't be read.
[[nodiscard]] std::optional<bool> isPlainFolder(HANDLE handle);

/// Whether the owner of @p handle (opened with READ_CONTROL) is SYSTEM, TrustedInstaller or BUILTIN\Administrators;
/// nullopt when it can't be read (WindowsSecurityOwners.h). A folder made while elevated is owned by Administrators,
/// %ProgramData% by SYSTEM and Windows' own folders by TrustedInstaller; one a user made is none of them, and only an
/// administrator can give a folder any of these owners (#4699). ElevatedRestart's checkConfigDir() and
/// WindowsDumpFolder's folderReaders() use it too (#4715).
[[nodiscard]] std::optional<bool> ownerIsSystemOrAdministratorsOrTrustedInstaller(HANDLE handle);

/// A security descriptor (LocalFree'd when it dies).
using OwnedSecurityDescriptor = std::unique_ptr<void, decltype(&::LocalFree)>;
/// The security descriptor @p sddl describes, or null when it doesn't parse.
[[nodiscard]] OwnedSecurityDescriptor securityDescriptorFrom(std::string_view sddl);

/// The folder @p folder opened by its path to be a held parent (FILE_LIST_DIRECTORY and FILE_READ_ATTRIBUTES, and
/// @p extraAccess, such as READ_CONTROL to read its DACL; shared read and write, never delete), a reparse point opened
/// itself, never followed: callers check isPlainFolder(). The error is GetLastError()'s message.
[[nodiscard]] std::expected<UniqueHandle, std::string> openFolderByPath(const std::filesystem::path& folder, ACCESS_MASK extraAccess = 0);

/// Whether two paths are the same text, ignoring case as NTFS compares names and trailing separators except a drive
/// root's (samePathIgnoringTrailingSeparators(), OrdinalCase.h, #4699): a held folder's finalPathOf() against the path
/// it was opened by.
[[nodiscard]] bool samePathText(std::wstring_view left, std::wstring_view right) noexcept;

/// Whether an existing folder's owner (read through the handle) is trusted to take it over in place; nullopt when the
/// owner can't be read.
using OwnerTrustedFn = std::function<std::optional<bool>(HANDLE)>;

/// Folder @p name, under the held @p parent, secured to @p security's DACL and owner, and held open (READ_CONTROL,
/// WRITE_DAC, FILE_LIST_DIRECTORY, SYNCHRONIZE and FILE_READ_ATTRIBUTES, without FILE_SHARE_DELETE). It is made fresh
/// when it isn't there. An entry there already is taken over in place only when it is a plain folder whose owner
/// @p ownerTrusted trusts (unset: ownerIsSystemOrAdministratorsOrTrustedInstaller()) and this process can open for
/// WRITE_DAC (its DACL reset, not its children's: SetKernelObjectSecurity doesn't propagate). Anything else -- a
/// user-owned folder, a protected DACL this process can't open for WRITE_DAC, a junction or a planted file -- is moved
/// aside (renameAside()) through a handle opened for DELETE alone, which the parent's FILE_DELETE_CHILD grants even when
/// the entry's own DACL grants this process nothing, and a fresh one is made in its place. @p display names it for the
/// error.
[[nodiscard]] std::expected<UniqueHandle, std::string> secureChildFolder(HANDLE parent,
                                                                         const std::wstring& name,
                                                                         PSECURITY_DESCRIPTOR security,
                                                                         const std::string& display,
                                                                         const OwnerTrustedFn& ownerTrusted = {});

} // namespace Platform::Windows

#endif // _WIN32
