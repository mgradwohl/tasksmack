#pragma once

// An elevated session's dump folder (#4164). The folder the administrator chose with Save dump to... is opened once by
// its path and held: it must be a plain folder (never a junction, link or other reparse point), and the folder Windows
// says the handle holds (GetFinalPathNameByHandleW) must be the one the path names, so a junction anywhere on its path
// is refused too. The dump file is then made relative to that handle (HandleRelativeFile.h's openRelative(),
// FILE_CREATE), never by its path, so a junction or object-manager link swapped in after the pick is never followed.
//
// The confirm warns when programs running as the signed-in user could read the dump (newFileReaders()): a simple,
// conservative read of the folder's owner and DACL, not an access check. It flags:
//   - a folder with no DACL (everyone has every access), or one owned by the signed-in user (who can change it);
//   - an allow ACE that new files inherit (OBJECT_INHERIT_ACE) granting read (FILE_READ_DATA, which is also
//     FILE_LIST_DIRECTORY, GENERIC_READ or GENERIC_ALL) to Everyone, Users, Authenticated Users, INTERACTIVE or the
//     signed-in user's own SID (this process's token user, which UAC's elevated token shares).
// It doesn't look at deny ACEs (so it may warn where a deny would stop the read), at object or callback ACEs, at
// other groups the user is in (a domain group, say), at CREATOR OWNER, or at who can read the folder's existing files.
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
#include <string>
#include <vector>

namespace Platform::Windows
{

// NOLINTBEGIN(misc-include-cleaner) - Win32 types come from windows.h sub-headers

/// @p folder (absolute) opened by its path and held (FILE_LIST_DIRECTORY, FILE_READ_ATTRIBUTES and READ_CONTROL, shared
/// read and write, never delete, so it can't be renamed or removed while held), checked as this file's header says. The
/// error says why it is refused, in the user's terms.
[[nodiscard]] std::expected<UniqueHandle, std::string> openHeldDumpFolder(const std::filesystem::path& folder);

/// Who can read a new file made in a folder with security descriptor @p descriptor (its owner and DACL), as this file's
/// header says; @p user is the signed-in user's SID (null: not looked for). AdministratorsOnly or ReadableByUser;
/// Unknown when the DACL can't be read from it.
[[nodiscard]] DumpFolderAccess newFileReaders(PSECURITY_DESCRIPTOR descriptor, PSID user);

/// newFileReaders() of the folder @p folder holds (opened with READ_CONTROL); Unknown when its security can't be read.
/// (Named apart from it: HANDLE and PSECURITY_DESCRIPTOR are both void*.)
[[nodiscard]] DumpFolderAccess heldFolderReaders(HANDLE folder, PSID user);

/// This process's token user, as a SID (in a buffer of its own); empty when it can't be read.
[[nodiscard]] std::vector<BYTE> currentUserSid();

/// openHeldDumpFolder() of @p folder, and heldFolderReaders() of it for currentUserSid(): Refused, with the reason, when it
/// can't be held.
[[nodiscard]] DumpFolderCheck checkDumpFolder(const std::filesystem::path& folder);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
