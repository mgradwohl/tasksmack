#pragma once

// An elevated session's dump folder (#4164). The folder the administrator chose with Save dump to... is opened once by
// its path and held. It must be a folder that is no junction, link or other reparse point, except a OneDrive (cloud
// files) folder, whose reparse point is the sync engine's placeholder, not a redirection (acceptedDumpFolder()). The
// folder Windows says the handle holds (GetFinalPathNameByHandleW) must be the one the path names, so a junction
// anywhere on its path, or a mapped or substituted drive, is refused too. The dump file is then made relative to that
// handle (HandleRelativeFile.h's openRelative(), FILE_CREATE), never by its path, so a junction or object-manager link
// swapped in after the pick is never followed.
//
// The confirm warns when programs not running as an administrator could read the dump (newFileReaders()). It is an
// allowlist read of the folder's owner and DACL, not an access check: only Administrators (BA), SYSTEM (SY) and
// TrustedInstaller are trusted, and CREATOR OWNER only where this token's default owner is Administrators. It flags:
//   - a folder with no DACL (everyone has every access);
//   - an owner other than Administrators or SYSTEM (as ownerIsAdministratorsOrSystem() trusts), who can change the
//     DACL new files inherit;
//   - an allow ACE new files inherit (OBJECT_INHERIT_ACE) granting read (FILE_READ_DATA, which is also
//     FILE_LIST_DIRECTORY, GENERIC_READ or GENERIC_ALL) to any SID not trusted -- the signed-in user, a group they are
//     in, or the user whose credentials weren't the ones that elevated;
//   - an allow ACE on the folder itself granting WRITE_DAC, WRITE_OWNER or GENERIC_ALL to any SID not trusted.
// A folder with no inheritable allow ACE at all is Unknown: a new file then gets the token's default DACL, which isn't
// read. Deny ACEs are ignored (so it may warn where a deny would stop the read); an object ACE that grants an
// inheritable read is flagged whatever its SID. Who can read the folder's existing files isn't looked at.
// A warning, never a refusal: the administrator chose the folder.

#include "Platform/IProcessActions.h"
#include "WindowsHandles.h"

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // NOLINT(misc-include-cleaner) - umbrella header; symbols reside in sub-headers
// clang-format on

#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// Whether a folder with @p attributes and reparse tag @p reparseTag (FILE_ATTRIBUTE_TAG_INFO) may hold an elevated
/// dump: a folder, and no reparse point, or one of Microsoft's cloud files tags (IO_REPARSE_TAG_CLOUD and its 16
/// variants, (tag & 0xFFFF0FFF) == IO_REPARSE_TAG_CLOUD: OneDrive). Only for dumps: PinnedFolder and DebugLogFile keep
/// isPlainFolder(), which refuses every reparse point.
[[nodiscard]] bool acceptedDumpFolder(DWORD attributes, DWORD reparseTag) noexcept;

/// @p folder (absolute) opened by its path and held (FILE_LIST_DIRECTORY, FILE_READ_ATTRIBUTES and READ_CONTROL, shared
/// read and write, never delete, so it can't be renamed or removed while held), checked as this file's header says. The
/// error says why it is refused, in the user's terms.
[[nodiscard]] std::expected<UniqueHandle, std::string> openHeldDumpFolder(const std::filesystem::path& folder);

/// Who can read a new file made in a folder with security descriptor @p descriptor (its owner and DACL), as this file's
/// header says. @p creatorOwnerIsAdministrators: whether CREATOR OWNER is trusted (this token's default owner is
/// Administrators, so a new file's owner is). AdministratorsOnly or ReadableByUser; Unknown when the DACL can't be read,
/// or no allow ACE is inherited by a new file.
[[nodiscard]] DumpFolderAccess newFileReaders(PSECURITY_DESCRIPTOR descriptor, bool creatorOwnerIsAdministrators);

/// newFileReaders() of the folder @p folder holds (opened with READ_CONTROL); Unknown when its security can't be read.
/// (Named apart from it: HANDLE and PSECURITY_DESCRIPTOR are both void*.)
[[nodiscard]] DumpFolderAccess heldFolderReaders(HANDLE folder, bool creatorOwnerIsAdministrators);

/// Whether this process's token's default owner (TokenOwner) is BUILTIN\Administrators; nullopt when it can't be read.
[[nodiscard]] std::optional<bool> defaultOwnerIsAdministrators();

/// openHeldDumpFolder() of @p folder, and heldFolderReaders() of it with defaultOwnerIsAdministrators(): Refused, with the
/// reason, when it can't be held; Unknown when the token can't be read.
[[nodiscard]] DumpFolderCheck checkDumpFolder(const std::filesystem::path& folder);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
