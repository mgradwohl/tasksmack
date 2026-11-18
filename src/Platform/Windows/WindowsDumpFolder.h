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
// TrustedInstaller are trusted, and CREATOR OWNER in an inherited ACE. It flags:
//   - a folder with no DACL (everyone has every access);
//   - any folder, when this token's default owner (a new file's owner, who always gets READ_CONTROL and WRITE_DAC on it)
//     isn't Administrators or SYSTEM;
//   - an owner other than Administrators or SYSTEM (as ownerIsAdministratorsOrSystem() trusts), who can change the
//     DACL new files inherit;
//   - an allow ACE new files inherit (OBJECT_INHERIT_ACE) granting read (FILE_READ_DATA, which is also
//     FILE_LIST_DIRECTORY, FILE_EXECUTE, GENERIC_READ, GENERIC_EXECUTE or GENERIC_ALL), WRITE_DAC or WRITE_OWNER to any SID
//     not trusted -- the signed-in user, a group they are
//     in, or the user whose credentials weren't the ones that elevated;
//   - an allow ACE on the folder itself granting WRITE_DAC, WRITE_OWNER or GENERIC_ALL to any SID not trusted.
// A folder with no inheritable allow ACE at all is Unknown: a new file then gets the token's default DACL, which isn't
// read. Deny, audit and alarm ACEs are ignored (so it may warn where a deny would stop the read); an object ACE that
// grants an inheritable read or control is flagged whatever its SID, and any other allow ACE type is Unknown. Who can read the folder's
// existing files isn't looked at. A warning, never a refusal: the administrator chose the folder.
//
// The dump file itself is admin-only wherever it is made (#4188): it is created with ADMIN_DUMP_FILE_SECURITY, its own
// protected DACL for Administrators and SYSTEM, owned by Administrators, as Windows protects its own privileged dumps
// (MEMORY.DMP, LiveKernelReports). Nothing is inherited from the folder, so the warning above is about the folder: its
// listing, the other files in it, and what syncs it (OneDrive uploads a dump saved there).
//
// The elevated default folder is %ProgramData%\TaskSmack\Dumps (makeAdminDumpFolder(), #4188), made and checked as the
// elevated log's %ProgramData%\TaskSmack\Logs is (#4105): each folder with secureChildFolder() and
// ADMIN_DUMP_FOLDER_SECURITY, so it passes the check above as AdministratorsOnly.

#include "HandleRelativeFile.h"
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
#include <string_view>

namespace Platform::Windows
{

/// The security an elevated session's dump file is made with, in any folder (#4188; SDDL): owned by Administrators, with
/// a protected DACL (nothing inherited from the folder) that gives Administrators and SYSTEM full control and no one else
/// any access.
inline constexpr std::string_view ADMIN_DUMP_FILE_SECURITY = "O:BAD:P(A;;FA;;;BA)(A;;FA;;;SY)";

/// The security the elevated default dump folder, and %ProgramData%\TaskSmack above it, are made with (#4188; SDDL): as
/// the elevated log's folders (App::DebugLogFile::ADMIN_SECURITY), owned by Administrators, a protected DACL for
/// Administrators and SYSTEM, inherited by what is made there.
inline constexpr std::string_view ADMIN_DUMP_FOLDER_SECURITY = "O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";

/// How makeAdminDumpFolder() makes the folder: the real security, or a test's.
struct AdminDumpFolderOptions
{
    /// The folders' security (SDDL). A test's leaves out the owner, which only an elevated token may set to
    /// Administrators, and adds an account of its own, so it can work in and remove the folders it made.
    std::string folderSecurity = std::string(ADMIN_DUMP_FOLDER_SECURITY);
    /// Whether a folder there already may be taken over in place (secureChildFolder()); unset: owned by Administrators
    /// or SYSTEM.
    OwnerTrustedFn ownerTrusted{}; // NOLINT(readability-redundant-member-init) - designated initializers leave it out
};

/// An elevated session's default dump folder (#4188), under @p programData (%ProgramData%, from its known folder):
/// TaskSmack\Dumps. @p programData is opened by its path once, as a plain folder; TaskSmack and Dumps are each made or
/// taken over relative to the one above with secureChildFolder() and @p options' security, as the elevated log's folders
/// are (#4105). The folder as Windows names it (its final path, so openHeldDumpFolder() takes it), or why it couldn't be
/// made.
[[nodiscard]] std::expected<std::filesystem::path, std::string> makeAdminDumpFolder(const std::filesystem::path& programData,
                                                                                    const AdminDumpFolderOptions& options = {});

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
/// header says. @p defaultOwnerTrusted: whether this token's default owner, a new file's owner, is Administrators or SYSTEM;
/// ReadableByUser when it isn't. AdministratorsOnly or ReadableByUser; Unknown when the DACL can't be read, an allow ACE
/// type isn't one it reads, or no allow ACE is inherited by a new file.
[[nodiscard]] DumpFolderAccess newFileReaders(PSECURITY_DESCRIPTOR descriptor, bool defaultOwnerTrusted);

/// newFileReaders() of the folder @p folder holds (opened with READ_CONTROL); Unknown when its security can't be read.
/// (Named apart from it: HANDLE and PSECURITY_DESCRIPTOR are both void*.)
[[nodiscard]] DumpFolderAccess heldFolderReaders(HANDLE folder, bool defaultOwnerTrusted);

/// Whether this process's token's default owner (TokenOwner), which owns a file it makes, is BUILTIN\Administrators or
/// SYSTEM; nullopt when it can't be read.
[[nodiscard]] std::optional<bool> defaultOwnerIsAdministratorsOrSystem();

/// openHeldDumpFolder() of @p folder, and heldFolderReaders() of it with defaultOwnerIsAdministratorsOrSystem(): Refused, with the
/// reason, when it can't be held; Unknown when the token can't be read.
[[nodiscard]] DumpFolderCheck checkDumpFolder(const std::filesystem::path& folder);

// NOLINTEND(misc-include-cleaner)

} // namespace Platform::Windows
