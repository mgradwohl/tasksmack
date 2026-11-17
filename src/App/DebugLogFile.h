#pragma once

// tasksmack-debug.log (#915): where it is, and how it is opened (#4105).
//   - A session whose environment is its own (not elevated, or a CI runner's Default token): the temp folder's
//     tasksmack-debug.log, opened by its path and truncated, as spdlog opens a file (Core::makeLogFileSink()).
//   - Windows, a session whose environment may be the less-privileged side's (elevated through UAC, or restarted with
//     --handoff: DllSearchPolicy::system32OnlyForThisProcess()): TEMP is that side's, so a junction to \RPC Control and an
//     object-manager link named tasksmack-debug.log could point the truncation at any file. The log goes to
//     %ProgramData%\TaskSmack\Logs instead, %ProgramData% read from its known folder, never the environment. Both
//     folders are made, or taken over if already there, with ADMIN_SECURITY: owned by Administrators, and only
//     Administrators and SYSTEM may change anything in them. Each is checked through a handle to be a plain folder, no
//     junction, link or other reparse point, at its own final path. The log is then opened without following a link,
//     checked through its handle to be a plain file with one link at the path asked for, and only then emptied.
//     Anything that fails a check is refused: the log goes to the debugger and the console only, and says why there.
//
// The folders and their security are seams, so test_DebugLogFile.cpp drives each case in a temp folder of its own,
// with junctions and hard links any user can make.

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

/// The security an elevated session's log folders get (SDDL): owned by Administrators, with a protected DACL (nothing
/// inherited from %ProgramData%, where Users may create) that gives Administrators and SYSTEM full control, inherited
/// by the log, and no one else any access.
inline constexpr std::string_view ADMIN_SECURITY = "O:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";

/// The temp folder: std::filesystem::temp_directory_path(). Throws, as that does, when there is none.
[[nodiscard]] std::filesystem::path systemTempFolder();

/// %ProgramData% from its known folder (FOLDERID_ProgramData), never the environment; nullopt when it can't be read,
/// and on Linux.
[[nodiscard]] std::optional<std::filesystem::path> systemProgramDataFolder();

/// Where an elevated session's log goes, under @p programData: TaskSmack\Logs.
[[nodiscard]] std::filesystem::path elevatedLogFolder(const std::filesystem::path& programData);

/// The calls open() makes: the system's, or a test's.
struct Functions
{
    std::function<std::filesystem::path()> tempFolder = &systemTempFolder;
    std::function<std::optional<std::filesystem::path>()> programDataFolder = &systemProgramDataFolder;
    /// The security the elevated log's folders are made with, or given (SDDL). A test's may leave out the owner, which
    /// only an elevated token may set to Administrators.
    std::string folderSecurity = std::string(ADMIN_SECURITY);
};

/// The log file opened.
struct Opened
{
    std::filesystem::path path; ///< Where it is, for the log's first line
    spdlog::sink_ptr sink;
};

/// tasksmack-debug.log, emptied, as this file's header says: in the temp folder by its path, or, on Windows with
/// @p lessPrivilegedEnvironment, in %ProgramData%\TaskSmack\Logs, only once each folder and the file pass their checks.
/// The error says why it wasn't opened.
[[nodiscard]] std::expected<Opened, std::string> open(bool lessPrivilegedEnvironment, const Functions& functions = {});

#ifdef _WIN32
/// Folder @p folder, whose parent is there, made with security @p sddl, or given it (its owner, when @p sddl names
/// one, and its DACL) when it is there already; then checked through a handle that doesn't follow a link to be a plain
/// folder, no reparse point, at its own final path. The error says which step failed.
[[nodiscard]] std::expected<void, std::string> secureFolder(const std::filesystem::path& folder, std::string_view sddl);
#endif

} // namespace App::DebugLogFile
