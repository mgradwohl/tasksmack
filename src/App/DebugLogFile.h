#pragma once

// tasksmack-debug.log (#915): where it is, and how it is opened (#4105).
//   - A session whose environment is its own (not elevated, or a CI runner's Default token): the temp folder's
//     tasksmack-debug.log, opened by its path and truncated, as spdlog opens a file (Core::makeLogFileSink()).
//   - Windows, a session whose environment may be the less-privileged side's (elevated through UAC, or restarted with
//     --handoff: DllSearchPolicy::system32OnlyForThisProcess()): TEMP is that side's, so a junction to \RPC Control and an
//     object-manager link named tasksmack-debug.log could point the truncation at any file. The log goes to
//     %ProgramData%\TaskSmack\Logs instead, %ProgramData% read from its known folder, never the environment.
//   - Linux, any root session (#4150): sudo -E, su -m and doas keep the user's TMPDIR, so a link there
//     could point the truncation at any file root can write. The log goes to /run/tasksmack instead, opened as
//     open()'s comment says.
//
// How that elevated log is opened, so a less-privileged user can neither read it, own it, nor send it elsewhere, with
// no race (the review of #4138):
//   - %ProgramData% is opened by path, once, as a plain folder (no reparse point). Every folder and the file below it
//     is then opened relative to the handle above it (NtCreateFile with RootDirectory), never by re-walking the path, so
//     nothing swapped in between steps is followed.
//   - TaskSmack and Logs: created fresh with ADMIN_SECURITY (owned by Administrators, a protected DACL for
//     Administrators and SYSTEM only). An entry there already is taken over in place only when it is a plain folder that
//     Administrators or SYSTEM owns and this process can open for WRITE_DAC: its DACL is reset to ADMIN_SECURITY
//     (SetKernelObjectSecurity, which doesn't propagate to children). Anything else -- a user-owned folder, a protected
//     DACL this process can't open for WRITE_DAC, a junction or a planted file -- is moved aside (a random
//     TaskSmack.untrusted-<n>) through a handle opened for DELETE alone, granted by the parent folder's
//     FILE_DELETE_CHILD, and a fresh one is made in its place, or refused when it can't be (a user can block the move by
//     holding the entry, or a file inside it, open without FILE_SHARE_DELETE, or by holding an oplock on it). Each folder
//     is held open, without FILE_SHARE_DELETE, until the file is. The in-place takeover is not claimed to be unraceable:
//     a handle a user opened on an Administrators-owned folder before its DACL was reset can still add or change entries
//     in it while it is held, or remove the log after the run; it can't read the log or redirect it, since the log file
//     below carries its own ADMIN_SECURITY and is never reused.
//   - The log file: any existing one is opened without following a link and deleted through that handle (POSIX
//     semantics, or delete-on-close), then a new one is created with CREATE-only disposition and ADMIN_SECURITY. A file
//     that can't be made fresh (a race re-made it) is refused. It is never reused, so a planted file with the user's own
//     DACL is never written to.
//   - Anything that fails a check is refused: the log goes to the debugger and the console only, and says why there.
//
// The handle-relative opens, the move aside, the delete through a handle and the folder takeover are
// Platform::Windows' (Platform/Windows/HandleRelativeFile.h, #4143), shared with the handed-over config folder
// (PinnedFolder); this file says which folders and file they make, and with what security.
//
// The folders, their security, the owner trust and the final-path read are seams, so test_DebugLogFile.cpp drives each case in a temp
// folder of its own, with junctions and hard links any user can make.

#include <spdlog/common.h>
#include <spdlog/logger.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace App::DebugLogFile
{

/// The log's file name.
inline constexpr std::string_view FILE_NAME = "tasksmack-debug.log";

/// A Linux root session's log name in /run/tasksmack, one per user it was started for (#4261): two root sessions for
/// different users (multi-seat, X forwarding) each take their own instance lock, so with one name the second removed
/// the first one's live log. "tasksmack-debug-<uid>.log" for @p invokingUid, "tasksmack-debug-root.log" for none.
[[nodiscard]] std::string rootLogName(std::optional<std::uint32_t> invokingUid);

/// The user a Linux root session was started for (SessionUser::processInvokingUser()), or nullopt: root's own, and
/// every other platform.
[[nodiscard]] std::optional<std::uint32_t> systemInvokingUid();

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
/// A handle's final path (GetFinalPathNameByHandleW), or empty when it can't be read: a seam the tests drive to steer
/// the reported path of the opened log.
using FinalPathFn = std::function<std::wstring(void* /*handle*/)>;
/// Whether an existing folder's owner (read through @p handle) is one this code trusts to take over in place: unset,
/// Administrators or SYSTEM. A seam, so a test can run the take-over-in-place path, and the stale-log delete under it,
/// without an elevated token. nullopt when the owner can't be read.
using OwnerTrustedFn = std::function<std::optional<bool>(void* /*handle*/)>;
[[nodiscard]] std::wstring systemFinalPath(void* handle);
#endif

/// Where a Linux root session keeps its log (#4150): the folder /run/tasksmack is made in. /run, not /var/log: it is
/// root:root 0755 on every systemd distribution, where Ubuntu's /var/log is root:syslog 0775, which the owner check
/// refuses (the #4165 review); and a tmpfs, cleared at boot, as a per-run debug log in the temp folder was.
inline constexpr std::string_view LINUX_LOG_PARENT = "/run";

/// The calls open() makes: the system's, or a test's.
struct Functions
{
    std::function<std::filesystem::path()> tempFolder = &systemTempFolder;
    /// Linux, less privileged (#4150): the folder the log's own folder, tasksmack, is made in.
    std::filesystem::path linuxLogParent = std::filesystem::path(LINUX_LOG_PARENT);
    /// Linux, less privileged: whose session this is, for the log's name (rootLogName(), #4261).
    std::function<std::optional<std::uint32_t>()> invokingUid = &systemInvokingUid;
    std::function<std::optional<std::filesystem::path>()> programDataFolder = &systemProgramDataFolder;
    /// The security the elevated log's folders and file are made with (SDDL). A test's may leave out the owner, which
    /// only an elevated token may set to Administrators, and add its own account, which only a test needs.
    std::string folderSecurity = std::string(ADMIN_SECURITY);
#ifdef _WIN32
    FinalPathFn finalPath = &systemFinalPath;
    /// Unset: Administrators or SYSTEM, read from the folder.
    OwnerTrustedFn ownerTrusted{}; // NOLINT(readability-redundant-member-init) - designated initializers leave it out
#endif
};

/// The log file opened.
struct Opened
{
    std::filesystem::path path; ///< Where it is, for the log's first line
    spdlog::sink_ptr sink;
};

/// tasksmack-debug.log, emptied, as this file's header says: in the temp folder by its path, or, with
/// @p lessPrivilegedEnvironment, on Windows in %ProgramData%\TaskSmack\Logs, made for Administrators and SYSTEM only,
/// and on Linux in /run/tasksmack (#4150): that folder made, or taken, only when the process's own user owns it
/// and it's a plain folder, mode 0700, every step relative to the folder above it with O_NOFOLLOW, and the file made
/// fresh (O_EXCL) after any old one is removed. The error says why it wasn't opened.
[[nodiscard]] std::expected<Opened, std::string> open(bool lessPrivilegedEnvironment, const Functions& functions = {});

#ifdef _WIN32
/// Folder @p folder secured as this file's header says, and held: made fresh with @p sddl, or, there already and owned
/// by Administrators or SYSTEM and openable for WRITE_DAC, its DACL reset; anything else (a user-owned folder, a
/// junction, a planted file) moved aside through a DELETE handle and remade. The error says which step failed. For
/// open() and its tests.
[[nodiscard]] std::expected<void, std::string> secureFolder(const std::filesystem::path& folder, std::string_view sddl);
#endif

// When the log's lines reach the disk (#4419). The temp folder's sink writes through a buffered FILE*, and spdlog flushes
// nothing unless it is told to, so a release build's file stayed empty until a clean exit: a crash, a kill from Task
// Manager or a hang lost the warnings and errors it was kept for. (The elevated and root sessions' sinks write each line
// straight through; flushing them costs nothing.)
//   - A line at the flush level or above is flushed as it is logged, on the thread that logs it: warn and up in a release
//     build, which at its default level (warn) is every line it logs; every line in a debug build. Warnings are rare, so
//     the UI thread's share is a few fflush() calls a session, as a debug build has always paid for every line.
//   - Everything else is flushed every FLUSH_INTERVAL on a worker of its own (Domain::Worker, LOG_FLUSH_THREAD_NAME),
//     never the UI thread, and only while the logger logs below the flush level (TASKSMACK_LOG_LEVEL=info, the resize
//     trace): otherwise nothing is ever left buffered, and an idle session gets no wake-up for it.
//   - A clean exit flushes once more when the policy goes (runApp()'s return or its exception), after the worker stops.

/// Lines at this level and above are flushed as they are logged in a release build.
inline constexpr spdlog::level::level_enum RELEASE_FLUSH_LEVEL = spdlog::level::warn;
/// A debug build flushes every line it logs (its level is debug).
inline constexpr spdlog::level::level_enum DEBUG_FLUSH_LEVEL = spdlog::level::debug;
/// How often the worker flushes the lines below the flush level: a crash loses at most this much of them.
inline constexpr std::chrono::seconds FLUSH_INTERVAL{3};

/// The flush policy above, for one logger, for as long as it lives. Build it after the logger's level is set.
class FlushPolicy
{
  public:
    /// Flushes @p logger as each line at @p flushLevel or above is logged, and, when @p logger logs below
    /// @p flushLevel, all of it every @p interval on the worker. Throws std::bad_alloc.
    FlushPolicy(std::shared_ptr<spdlog::logger> logger,
                spdlog::level::level_enum flushLevel,
                std::chrono::milliseconds interval = FLUSH_INTERVAL);
    /// Stops the worker, waiting for a flush it is in, then flushes @p logger once more. The flush level stays.
    ~FlushPolicy();

    FlushPolicy(const FlushPolicy&) = delete;
    FlushPolicy& operator=(const FlushPolicy&) = delete;
    FlushPolicy(FlushPolicy&&) = delete;
    FlushPolicy& operator=(FlushPolicy&&) = delete;

    /// Whether the periodic flush runs: the logger logs below the flush level, and the worker took its job.
    [[nodiscard]] bool periodic() const noexcept;

  private:
    struct Flusher;
    std::shared_ptr<spdlog::logger> m_Logger;
    std::unique_ptr<Flusher> m_Flusher; ///< Null when nothing is left to flush periodically
};

} // namespace App::DebugLogFile
