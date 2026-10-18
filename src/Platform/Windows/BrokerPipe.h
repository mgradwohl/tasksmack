#pragma once

// The Windows transport of the privileged broker (#2311; #2181): BrokerProtocol frames over a named
// pipe, the twin of the Linux socketpair (Linux/BrokerSocket.h). The elevated helper is the server. It
// makes the pipe under the random name TaskSmack started it with:
//   - as the first and only instance (FILE_FLAG_FIRST_PIPE_INSTANCE, one instance), so a name somebody
//     already holds fails rather than being shared, and nobody can add a second instance to it;
//   - for local clients only (PIPE_REJECT_REMOTE_CLIENTS);
//   - with a protected DACL that lets in only TaskSmack's user SID, with just the rights a client needs
//     to read and write (not FILE_CREATE_PIPE_INSTANCE), and a medium mandatory label, so a medium
//     TaskSmack can open an elevated helper's pipe and nothing below medium can.
// Then each side checks that the other end is the process it expects: TaskSmack that the server is the
// helper it started (GetNamedPipeServerProcessId), the helper that the client is the TaskSmack that
// started it (GetNamedPipeClientProcessId). Each holds a handle to that process meanwhile, so its PID
// can't be reused.
//
// The pipe is in byte mode, and a frame is BrokerProtocol's own: a four-byte little-endian length, then
// that many bytes. A frame declared larger than MAX_MESSAGE_BYTES is refused before anything is
// allocated for it; a byte stream can't skip past it, so it ends the session. Every wait is bounded and
// looks at its stop token every 100 ms, as the socket's do.

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

#include "Platform/Windows/WindowsHandles.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::Windows::BrokerPipe
{

/// Each direction's pipe buffer. A frame larger than this is written as the reader takes it.
inline constexpr DWORD PIPE_BUFFER_BYTES = DWORD{64} << 10U;

/// The rights the DACL grants the client, and all it asks for: read and write the pipe, query it (for
/// GetNamedPipeServerProcessId), wait on it. Not FILE_CREATE_PIPE_INSTANCE (FILE_APPEND_DATA), not
/// WRITE_DAC, not READ_CONTROL.
inline constexpr DWORD CLIENT_ACCESS = FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;

/// A fresh pipe name, \\.\pipe\TaskSmack.Broker.<128 random bits in hex>, from the system's
/// cryptographic random number generator. The error says why there are no random bits.
[[nodiscard]] std::expected<std::wstring, std::string> makePipeName();

/// The user SID ("S-1-5-21-...") of @p token, opened with TOKEN_QUERY.
[[nodiscard]] std::expected<std::wstring, std::string> userSidOfToken(HANDLE token);

/// The user SID of @p process, opened with PROCESS_QUERY_LIMITED_INFORMATION: whom the helper lets in,
/// read from the TaskSmack that started it, which needn't be the helper's own user (#2181).
[[nodiscard]] std::expected<std::wstring, std::string> userSidOfProcess(HANDLE process);

/// The pipe's security descriptor in SDDL: a protected DACL with one ACE, CLIENT_ACCESS for
/// @p userSid, and a medium no-read-up, no-write-up label. Refuses anything that isn't a plain SID
/// string ("S-1-" and digits and dashes), so no SDDL alias such as "WD" (Everyone) or ACE text can
/// get in.
[[nodiscard]] std::expected<std::wstring, std::string> pipeSecurityDescriptor(std::wstring_view userSid);

/// The server's end of a new pipe called @p name (\\.\pipe\...), which only @p allowedUserSid may
/// open, as described above. Fails when a pipe of that name already exists.
[[nodiscard]] std::expected<UniqueHandle, std::string> createServerPipe(const std::wstring& name, std::wstring_view allowedUserSid);

/// Whether the client on @p pipe is process @p expectedPid.
[[nodiscard]] std::expected<void, std::string> checkClientProcess(HANDLE pipe, DWORD expectedPid);

/// Whether the server of @p pipe is process @p expectedPid.
[[nodiscard]] std::expected<void, std::string> checkServerProcess(HANDLE pipe, DWORD expectedPid);

/// Waits up to @p timeout for a client to open @p pipe (from createServerPipe), then checks it is
/// process @p expectedClientPid. A client that isn't is disconnected, and the error says so: the pipe
/// has no second instance for TaskSmack to try, so the helper gives up.
[[nodiscard]] std::expected<void, std::string>
acceptClient(HANDLE pipe, DWORD expectedClientPid, std::chrono::milliseconds timeout, const std::stop_token& stopToken);

/// Opens pipe @p name as its client once its server has made it, waiting up to @p timeout for that,
/// then checks its server is process @p expectedServerPid. @p serverProcess is that process, waited
/// on so a helper that exits before it makes the pipe isn't waited for. Fails at once when another
/// client already has the pipe.
[[nodiscard]] std::expected<UniqueHandle, std::string> connectToServer(const std::wstring& name,
                                                                       DWORD expectedServerPid,
                                                                       HANDLE serverProcess,
                                                                       std::chrono::milliseconds timeout,
                                                                       const std::stop_token& stopToken);

/// Writes one frame, waiting up to @p timeout for the reader to take it. False (and why in @p error)
/// when it is larger than MAX_MESSAGE_BYTES, the other end has gone, the write failed, or it wasn't
/// all taken in time. A frame part-written is a broken channel.
[[nodiscard]] bool sendFrame(HANDLE pipe,
                             std::span<const std::uint8_t> frame,
                             std::chrono::milliseconds timeout,
                             const std::stop_token& stopToken,
                             std::string& error);

/// What receiveFrame() found when there was no frame.
enum class ReceiveEnd : std::uint8_t
{
    Closed,   ///< The other end closed the pipe before a frame began: the normal end of a session
    TimedOut, ///< Nothing within the timeout
    Stopped,  ///< The stop token was stopped
    Failed,   ///< A read error, a frame larger than allowed, or a frame cut short
};

/// Waits up to @p timeout for one whole frame. A frame that began but didn't all come within it, or
/// whose writer closed the pipe partway, is Failed, not TimedOut or Closed: the stream is mid-frame
/// (#2406, #2451).
[[nodiscard]] std::expected<std::vector<std::uint8_t>, ReceiveEnd>
receiveFrame(HANDLE pipe, std::chrono::milliseconds timeout, const std::stop_token& stopToken);

/// The bytes waiting to be read on @p pipe, without reading them; nullopt once the other end has
/// closed it (or it can't be asked).
[[nodiscard]] std::optional<DWORD> bytesWaiting(HANDLE pipe) noexcept;

} // namespace Platform::Windows::BrokerPipe

#endif // _WIN32
