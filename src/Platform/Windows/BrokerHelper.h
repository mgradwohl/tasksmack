#pragma once

// TaskSmackHelper.exe's session (#2517; #2181): the elevated helper TaskSmack starts with
// ShellExecuteExW("runas") to do what it can't itself. Started as
//   TaskSmackHelper.exe --broker <pipe name> <TaskSmack's PID> <TaskSmack's start time>
// it:
//   - opens TaskSmack's process (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION), checks its start time
//     against the one given, and keeps that handle for the whole session, so the PID it checks the
//     pipe's client against can't name another process meanwhile;
//   - reads TaskSmack's user SID from TaskSmack's own token, never from the command line, and makes the
//     pipe for that user only (BrokerPipe::createServerPipe), so an over-the-shoulder elevation, which
//     runs the helper as another user, still lets in only the user who asked;
//   - accepts one client, which must be that process (BrokerPipe::acceptClient);
//   - serves it (BrokerPipeServer::serve) until it closes the pipe, the idle timeout passes, or
//     TaskSmack's process ends, whichever comes first.
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

#include "Platform/Windows/BrokerPipeServer.h"
#include "Platform/Windows/WindowsHandles.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace Platform::Windows::BrokerHelper
{

/// How long the helper waits for a user request before it exits; TaskSmack starts it again when needed.
inline constexpr std::chrono::minutes IDLE_TIMEOUT{5};

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

/// How the helper's run ended, and its exit status.
enum class Exit : std::uint8_t
{
    Done = 0,     ///< The session ended: the client closed the pipe, it was idle, or TaskSmack exited
    Failed = 1,   ///< The session broke, its handshake failed, or something unexpected went wrong
    Usage = 2,    ///< The command line isn't one TaskSmack makes
    NoParent = 3, ///< The process named isn't there, or isn't the one that started at the time given
    NoClient = 4, ///< The pipe couldn't be made, or its parent didn't open it in time (or something else did)
};

/// The whole session for @p arguments: open and check the parent, make its pipe, accept it, and serve
/// @p handler until the session ends (see above). @p acceptTimeout bounds the wait for the parent to
/// open the pipe; @p stopToken ends any of it early.
[[nodiscard]] Exit run(const Arguments& arguments,
                       const BrokerPipeServer::RequestHandler& handler,
                       std::chrono::milliseconds idleTimeout,
                       std::chrono::milliseconds acceptTimeout,
                       const std::stop_token& stopToken);

} // namespace Platform::Windows::BrokerHelper

#endif // _WIN32
