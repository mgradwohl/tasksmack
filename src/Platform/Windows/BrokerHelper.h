#pragma once

// TaskSmackHelper.exe's session (#2517; #2181): the elevated helper TaskSmack starts with
// ShellExecuteExW("runas") to do what it can't itself. Started as
//   TaskSmackHelper.exe --broker <pipe name> <TaskSmack's PID> <TaskSmack's start time>
// it:
//   - opens TaskSmack's process (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION), checks its start time
//     against the one given, and keeps that handle for the whole session, so the PID it checks the
//     pipe's client against can't name another process meanwhile;
//   - checks that process is TaskSmack, and the one that started it (#2549): it is running the
//     TaskSmack.exe in the helper's own folder, it is the helper's own parent, and it started first.
//     ShellExecuteExW("runas") makes the elevated process the caller's child, not consent.exe's or
//     AppInfo's: AppInfo creates it with PROC_THREAD_ATTRIBUTE_PARENT_PROCESS set to the caller's
//     process, so the caller is its parent both in ProcessBasicInformation and in Task Manager's tree;
//   - checks TaskSmack's user can't change that install: neither exe, their folder, nor any folder above
//     it (checkUnchangeableBy()). Without this, a program could copy the signed helper into a folder of
//     its own, save itself beside it as TaskSmack.exe, and through the helper's trusted UAC prompt get
//     an elevated broker. So the helper serves only an install an administrator made, such as one in
//     Program Files: a TaskSmack unpacked to a folder its user can write is refused.
//     This stops a program from using the helper on its own. It doesn't stop code that runs inside a
//     real TaskSmack, which any program of the same user can start and inject into;
//   - reads TaskSmack's user SID from TaskSmack's own token, never from the command line, and makes the
//     pipe for that user only (BrokerPipe::createServerPipe), so an over-the-shoulder elevation, which
//     runs the helper as another user, still lets in only the user who asked;
//   - accepts one client, which must be that process (BrokerPipe::acceptClient);
//   - serves it (BrokerPipeServer::serve) until it closes the pipe, the idle timeout passes, or
//     TaskSmack's process ends, whichever comes first;
//   - serves an over-the-shoulder elevation less (#2550; finding 2 of #2548's review): when the helper
//     runs as another account than TaskSmack's user (a standard user's TaskSmack, approved with an
//     administrator's password), the administrator approved one action, not a session. So the session
//     ends once one action is answered, waits OVER_THE_SHOULDER_IDLE_TIMEOUT at most for it, and reads
//     only TaskSmack's user's own processes (sessionPolicy()). Same-user elevation, where UAC isn't a
//     boundary anyway, is unchanged.
// The command line is untrusted: anything that doesn't parse exactly is refused before anything is
// opened. The arguments only say where to look; who is let in is decided by the parent process's
// handle and token.
//
// HelperMain.cpp is the executable; this is the part the tests run in-process.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include "Platform/IProcessActions.h"
#include "Platform/Windows/BrokerPipe.h"
#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Platform::Windows::BrokerHelper
{

/// How long the helper waits for a user request before it exits; TaskSmack starts it again when needed.
inline constexpr std::chrono::minutes IDLE_TIMEOUT{5};
/// How long an over-the-shoulder session waits for its one action (#2550): long enough to act on the
/// prompt just approved, short enough that the administrator's approval doesn't wait around for
/// whatever else runs as TaskSmack's user.
inline constexpr std::chrono::minutes OVER_THE_SHOULDER_IDLE_TIMEOUT{1};

/// The file name TaskSmack's executable has: the helper serves only the one beside it.
inline constexpr std::wstring_view TASKSMACK_FILE_NAME = L"TaskSmack.exe";

/// What the command line says: where to serve, and whom.
struct Arguments
{
    std::wstring pipeName;              ///< \\.\pipe\TaskSmack.Broker.<32 lowercase hex digits>
    DWORD parentPid = 0;                ///< TaskSmack's process ID, never 0
    std::uint64_t parentStartTicks = 0; ///< TaskSmack's creation time, as a FILETIME's 100 ns ticks, never 0
};

/// @p arguments (argv without the program's name) as Arguments: exactly "--broker", a pipe name
/// BrokerPipe::makePipeName() could have made, and two unsigned decimal numbers, neither 0. Anything
/// else (a sign, spaces, leading zeros, a number out of range, another pipe) is refused, and the error
/// says why.
[[nodiscard]] std::expected<Arguments, std::string> parseArguments(std::span<const std::wstring_view> arguments);

/// The parameters TaskSmack starts the helper with, the inverse of parseArguments(). None needs quoting:
/// the pipe name is hex digits and dots, the numbers are digits.
[[nodiscard]] std::wstring commandLine(const Arguments& arguments);

/// The process @p pid, if it is the one that started at @p startTicks, opened with SYNCHRONIZE and
/// PROCESS_QUERY_LIMITED_INFORMATION. The handle pins it, so its PID can't be reused while it is held.
[[nodiscard]] std::expected<UniqueHandle, std::string> openParent(DWORD pid, std::uint64_t startTicks);

/// @p fileName in the folder of @p exePath, an absolute path; empty when @p exePath isn't absolute or
/// has no folder. TaskSmack finds the helper this way and the helper TaskSmack, never through a search
/// path or the current directory.
[[nodiscard]] std::wstring besidePath(std::wstring_view exePath, std::wstring_view fileName);

/// Whether @p first and @p second name the same file: each is opened, and the final paths Windows
/// gives for the two handles are compared, ignoring case as NTFS does. So "..", 8.3 short names,
/// symbolic links, junctions and case don't matter. False when either can't be opened.
[[nodiscard]] bool sameFile(const std::wstring& first, const std::wstring& second);

/// Rights over an installed exe that would let a user put another program in its place.
inline constexpr DWORD FILE_CHANGE_RIGHTS = FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER;
/// Rights over the install's folder that would let a user add, remove or rename what is in it.
inline constexpr DWORD FOLDER_CHANGE_RIGHTS = FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
/// Rights over each folder above it that would let a user move the install's folder, or one above it,
/// out of the way. Not FILE_ADD_FILE or FILE_ADD_SUBDIRECTORY: a new entry can't take an existing
/// one's name, and every user may create folders at a drive's root.
inline constexpr DWORD ANCESTOR_CHANGE_RIGHTS = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;

/// Whether the user of @p token (an impersonation token, identification level is enough) can change
/// none of @p files: no FILE_CHANGE_RIGHTS over any file, no FOLDER_CHANGE_RIGHTS over the folder of
/// each, and no ANCESTOR_CHANGE_RIGHTS over any folder above, up to the root. Checked with AccessCheck
/// against each path's security descriptor, after links are resolved. WRITE_DAC is among the rights,
/// so an owner's implicit rights count too. Anything that can't be read is a refusal. The error names
/// the path.
[[nodiscard]] std::expected<void, std::string> checkUnchangeableBy(HANDLE token, std::span<const std::wstring> files);

/// checkUnchangeableBy() with @p parent's own token, duplicated for identification: TaskSmack's user.
[[nodiscard]] std::expected<void, std::string> checkUnchangeableByParent(HANDLE parent, std::span<const std::wstring> files);

/// What the helper knows of itself, that its parent is checked against.
struct Self
{
    using InstallCheck = std::expected<void, std::string> (*)(HANDLE parent, std::span<const std::wstring> files);

    std::wstring taskSmackPath;   ///< The TaskSmack.exe its parent must be running: the one beside the helper
    DWORD creatorPid = 0;         ///< The process that created the helper (ProcessBasicInformation's parent)
    std::uint64_t startTicks = 0; ///< The helper's own creation time, in a FILETIME's ticks
    std::wstring helperPath;      ///< The helper's own image
    /// Whether the parent's user can't change the install: checkUnchangeableByParent(), or a test's.
    InstallCheck installCheck = &checkUnchangeableByParent;
};

/// This process's Self: besidePath(its own image, TASKSMACK_FILE_NAME), its parent's PID, its creation
/// time and its own image. The error says which couldn't be read.
[[nodiscard]] std::expected<Self, std::string> readSelf();

/// Whether @p parent, opened by openParent() for @p arguments, is the TaskSmack that started this
/// helper from an install its user can't change: @p self's creator is the PID named, that process
/// started no later than the helper (so its PID wasn't reused since), its image is @p self's
/// taskSmackPath (sameFile()), and @p self's installCheck passes for that and the helper's image. The
/// error says which check failed.
[[nodiscard]] std::expected<void, std::string> checkParentIsTaskSmack(HANDLE parent, const Arguments& arguments, const Self& self);

/// A stop token that is stopped when @p process ends or @p outer is stopped: the session's, so the
/// helper exits with TaskSmack. @p process (any waitable handle) must outlive this. Watched by a thread
/// that blocks on it, not a poll.
class ProcessWatch
{
  public:
    ProcessWatch(HANDLE process, const std::stop_token& outer);
    ~ProcessWatch();
    ProcessWatch(const ProcessWatch&) = delete;
    ProcessWatch& operator=(const ProcessWatch&) = delete;
    ProcessWatch(ProcessWatch&&) = delete;
    ProcessWatch& operator=(ProcessWatch&&) = delete;

    [[nodiscard]] std::stop_token token() const noexcept
    {
        return m_Stop.get_token();
    }

  private:
    std::stop_source m_Stop;
    UniqueHandle m_Wake; // set to end the watching thread
    std::stop_callback<std::function<void()>> m_Outer;
    std::jthread m_Thread; // last: joined first, while the rest is alive
};

/// What one consent lets TaskSmack's user do through the helper (#2550).
struct SessionPolicy
{
    BrokerPipeServer::Grant grant = BrokerPipeServer::Grant::Session;
    std::chrono::milliseconds idleTimeout{IDLE_TIMEOUT};
    bool readsOwnProcessesOnly = false; ///< Process reads only of TaskSmack's user's processes, no system-wide reads
};

/// The session for TaskSmack's @p user from a helper running as @p helperUser, with @p idleTimeout as
/// a same-user session's idle timeout. The same account: a session, as asked. Another account (an
/// over-the-shoulder elevation): Grant::OneAction, the shorter of @p idleTimeout and
/// OVER_THE_SHOULDER_IDLE_TIMEOUT, and reads of TaskSmack's user's own processes only.
[[nodiscard]] SessionPolicy
sessionPolicy(const BrokerPipe::Sid& user, const BrokerPipe::Sid& helperUser, std::chrono::milliseconds idleTimeout);

/// Whether @p target, confirmed by its start time, runs as @p user: its token's user. False when that
/// can't be told (the process is gone or another one, or its token can't be read).
[[nodiscard]] bool runsAs(const ProcessTarget& target, const BrokerPipe::Sid& user);

/// The words a read refused by readsOnlyOwnProcesses() carries, for Process Details to show.
inline constexpr std::string_view OTHER_USERS_READ_REFUSED =
    "The administrator helper was approved for another account, so it reads only your own processes";

/// @p handler for an over-the-shoulder session (#2550): a read of a process that @p ownedByUser says
/// isn't TaskSmack's user's is refused with OTHER_USERS_READ_REFUSED, as a failed read of its kind, and
/// never reaches @p handler; a system-wide read (firmware, crashes) is refused outright. The Hello and
/// actions pass through.
[[nodiscard]] BrokerPipeServer::RequestHandler readsOnlyOwnProcesses(BrokerPipeServer::RequestHandler handler,
                                                                     std::function<bool(const ProcessTarget&)> ownedByUser);

/// How the helper's run ended, and its exit status.
enum class Exit : std::uint8_t
{
    Done = 0,     ///< The session ended: the client closed the pipe, it was idle, TaskSmack exited, or its one action was answered
    Failed = 1,   ///< The session broke, its handshake failed, or something unexpected went wrong
    Usage = 2,    ///< The command line isn't one TaskSmack makes
    NoParent = 3, ///< The process named isn't there, isn't the one that started at the time given, isn't the
                  ///< TaskSmack beside the helper that started it (checkParentIsTaskSmack()), or isn't one user's
    NoClient = 4, ///< The pipe couldn't be made, or its parent didn't open it in time (or something else did)
};

/// What exit status @p code of TaskSmackHelper.exe means, in TaskSmack's words, for the error
/// TaskSmack shows when the helper exits before it opens its pipe.
[[nodiscard]] std::string exitMeaning(DWORD code);

/// The whole session for @p arguments: open the parent and check it against @p self, make its pipe,
/// accept it, and serve @p handler until the session ends (see above), as sessionPolicy() says for the
/// parent's user and this process's own. @p acceptTimeout bounds the wait for the parent to open the
/// pipe; @p stopToken ends any of it early.
[[nodiscard]] Exit run(const Arguments& arguments,
                       const Self& self,
                       const BrokerPipeServer::RequestHandler& handler,
                       std::chrono::milliseconds idleTimeout,
                       std::chrono::milliseconds acceptTimeout,
                       const std::stop_token& stopToken);

/// run() once the parent is open and its user read: refuses a @p user that checkUserSid() doesn't take
/// (a service's, LocalSystem's, a group) as NoParent, then makes pipe @p pipeName for that user, accepts
/// @p parent as its client and serves it, under sessionPolicy(@p user, @p helperUser, @p idleTimeout).
/// @p parent must stay open until this returns.
[[nodiscard]] Exit serveParent(HANDLE parent,
                               const BrokerPipe::Sid& user,
                               const BrokerPipe::Sid& helperUser,
                               const std::wstring& pipeName,
                               const BrokerPipeServer::RequestHandler& handler,
                               std::chrono::milliseconds idleTimeout,
                               std::chrono::milliseconds acceptTimeout,
                               const std::stop_token& stopToken);

/// SetProcessMitigationPolicy(), or a test's stand-in that records what it is asked for: true when
/// the policy was set.
using SetMitigationPolicy = std::function<bool(PROCESS_MITIGATION_POLICY policy, void* value, std::size_t size)>;

/// Turns on, through @p set, the process mitigations an elevated process that only serves a pipe can
/// live with (#2557), each on its own:
///   - image loads: no image from a remote (UNC) path or with a low mandatory label, and System32's
///     copy of a DLL before any other;
///   - extension points off: no AppInit DLLs, legacy IMEs, winevent hooks or the like loaded into it;
///   - strict handle checks: using a handle that isn't open raises an exception instead of reaching
///     whatever object holds that value later;
///   - no dynamic code: no page is made executable, or an executable one writable, after start;
///   - system fonts only: the helper draws nothing;
///   - redirection trust: no junction a non-administrator made is followed.
/// HelperMain calls it first, before any DLL loads. Not the Win32k system-call filter: terminate's
/// WM_CLOSE (EnumWindows, PostMessageW) needs Win32k. A policy Windows refuses (an older Windows that
/// doesn't know it) is skipped. Returns the names of the ones refused, for the log.
[[nodiscard]] std::vector<std::string_view> applyMitigations(const SetMitigationPolicy& set);

} // namespace Platform::Windows::BrokerHelper

#endif // _WIN32
