#pragma once

// tasksmack-debug.log (#915): where it is, and how it is opened (#4105).
//   - A session whose environment is its own (not elevated, or a CI runner's Default token): the temp folder's
//     tasksmack-debug.log, opened by its path and truncated, as spdlog opens a file (Core::makeLogFileSink()).
//   - Windows, a session whose environment may be the less-privileged side's (elevated through UAC, or restarted with
//     --handoff: DllSearchPolicy::system32OnlyForThisProcess()): TEMP is that side's, so a junction to \RPC Control and an
//     object-manager link named tasksmack-debug.log could point the truncation at any file. The log goes to
//     %ProgramData%\TaskSmack\Logs instead, %ProgramData% read from its known folder, never the environment.
//
// How that elevated log is opened, so a less-privileged user can neither read it, own it, nor send it elsewhere, with
// no race (the review of #4138):
//   - %ProgramData% is opened by path, once, as a plain folder (no reparse point). Every folder and the file below it
//     is then opened relative to the handle above it (NtCreateFile with RootDirectory), never by re-walking the path, so
//     nothing swapped in between steps is followed.
//   - TaskSmack and Logs: created fresh with ADMIN_SECURITY (owned by Administrators, a protected DACL for
//     Administrators and SYSTEM only), or, if there already and owned by Administrators or SYSTEM, their DACL reset to
//     that (SetKernelObjectSecurity, which doesn't propagate to children). A folder there already that a user owns is
//     renamed aside (TaskSmack.untrusted-<n>) through its own handle and a fresh one made in its place; a name that
//     still can't be created fresh is refused. Each folder is held open, without FILE_SHARE_DELETE, until the file is.
//   - The log file: any existing one is opened without following a link and deleted through that handle (POSIX
//     semantics), then a new one is created with CREATE-only disposition and ADMIN_SECURITY. A file that can't be made
//     fresh (a race re-made it) is refused. It is never reused, so a planted file with the user's own DACL is never
//     written to.
//   - Anything that fails a check is refused: the log goes to the debugger and the console only, and says why there.
//
// The folders, their security and the final-path read are seams, so test_DebugLogFile.cpp drives each case in a temp
// folder of its own, with junctions and hard links any user can make.

#include <spdlog/common.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace App::DebugLogFile
{

/// The log's file name.
inline constexpr std::string_view FILE_NAME = "tasksmack-debug.log";

/// The security an elevated session's log folders and file get (SDDL): owned by Administrators, with a protected DACL
/// (nothing inherited from %ProgramData%, where Users may create) that gives Administrators and SYSTEM full control,
/// inherited by the log, and no one else any access.
inline constexpr std::string_view ADMIN_SECURITY = "O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";

/// The temp folder: std::filesystem::temp_directory_path(). Throws, as that does, when there is none.
[[nodiscard]] std::filesystem::path systemTempFolder();

/// %ProgramData% from its known folder (FOLDERID_ProgramData), never the environment; nullopt when it can't be read,
/// and on Linux.
[[nodiscard]] std::optional<std::filesystem::path> systemProgramDataFolder();

/// Where an elevated session's log goes, under @p programData: TaskSmack\Logs.
[[nodiscard]] std::filesystem::path elevatedLogFolder(const std::filesystem::path& programData);

#ifdef _WIN32
/// A handle's final path (GetFinalPathNameByHandleW), or empty when it can't be read: a seam the tests drive to force
/// the final-path mismatch branch.
using FinalPathFn = std::function<std::wstring(void* /*handle*/)>;
[[nodiscard]] std::wstring systemFinalPath(void* handle);
#endif

/// The calls open() makes: the system's, or a test's.
struct Functions
{
    std::function<std::filesystem::path()> tempFolder = &systemTempFolder;
    std::function<std::optional<std::filesystem::path>()> programDataFolder = &systemProgramDataFolder;
    /// The security the elevated log's folders and file are made with (SDDL). A test's may leave out the owner, which
    /// only an elevated token may set to Administrators, and add its own account, which only a test needs.
    std::string folderSecurity = std::string(ADMIN_SECURITY);
#ifdef _WIN32
    FinalPathFn finalPath = &systemFinalPath;
#endif
};

/// The log file opened.
struct Opened
{
    std::filesystem::path path; ///< Where it is, for the log's first line
    spdlog::sink_ptr sink;
};

/// tasksmack-debug.log, emptied, as this file's header says: in the temp folder by its path, or, on Windows with
/// @p lessPrivilegedEnvironment, in %ProgramData%\TaskSmack\Logs, made for Administrators and SYSTEM only. The error
/// says why it wasn't opened.
[[nodiscard]] std::expected<Opened, std::string> open(bool lessPrivilegedEnvironment, const Functions& functions = {});

#ifdef _WIN32
/// Folder @p folder secured as this file's header says, and held: made fresh with @p sddl, or, there already and owned
/// by Administrators or SYSTEM, its DACL reset; a user-owned one renamed aside and remade. @p finalPath reads a handle's
/// final path. The error says which step failed. For open() and its tests.
[[nodiscard]] std::expected<void, std::string>
secureFolder(const std::filesystem::path& folder, std::string_view sddl, const FinalPathFn& finalPath = &systemFinalPath);
#endif

} // namespace App::DebugLogFile
