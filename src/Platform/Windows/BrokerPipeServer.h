#pragma once

// The privileged helper's side of the Windows broker transport (#2311): answers BrokerProtocol requests
// from TaskSmack over a BrokerPipe, with the session rules of the Linux helper (Linux/BrokerServer.h):
// a Hello of this protocol version first, an ErrorReply to a frame that doesn't decode, a too-large
// reply turned into an ErrorReply, and an idle exit only user requests put off. What a request does is
// the handler's: TaskSmackHelper.exe (#2517) hands it the shared request mapping and the Windows
// actions and readers.

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

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <stop_token>

namespace Platform::Windows::BrokerPipeServer
{

/// How long the helper waits for TaskSmack to open the pipe once it has made it.
inline constexpr std::chrono::seconds ACCEPT_TIMEOUT{30};
/// How long the helper waits for TaskSmack to take a reply before it gives up on the session.
inline constexpr std::chrono::seconds REPLY_TIMEOUT{30};

/// The answer to one request after the Hello. Whatever it throws is answered with an ErrorReply.
using RequestHandler = std::function<Broker::Response(const Broker::Request&)>;

/// How much one consent lets TaskSmack do through the session (#2550).
enum class Grant : std::uint8_t
{
    Session,   ///< Any number of requests, until the session ends: an elevation of TaskSmack's own user
    OneAction, ///< One request that isn't the Hello or a read, then the session ends: an over-the-shoulder elevation
};

/// Whether @p request is a read (of a process, or System Information's): every request kind that isn't
/// the Hello or an action. A new request kind must be sorted into one or the other here, which a
/// static_assert on the kinds' count enforces (#2550).
[[nodiscard]] bool isRead(const Broker::Request& request) noexcept;

/// Whether @p request is a process action Windows does: end, kill, a priority or an affinity. Not
/// Suspend, Resume or an I/O priority, which WindowsProcessActions always refuses.
[[nodiscard]] bool isActionWindowsDoes(const Broker::Request& request) noexcept;

/// Whether @p request puts off the Windows helper's idle exit: Broker::putsOffTheIdleExit()'s Hello
/// and actions, less the actions Windows always refuses (#2550; finding 2 of #2548's review). A
/// request that can only be refused does nothing for the user, so it doesn't keep an elevated helper
/// alive either.
[[nodiscard]] bool putsOffTheWindowsIdleExit(const Broker::Request& request) noexcept;

/// How a session ended, for the helper's exit status and the tests.
enum class SessionEnd : std::uint8_t
{
    ClientClosed,    ///< The client closed its end (or exited): the normal end
    Idle,            ///< No Hello or action within the idle timeout
    Stopped,         ///< The stop token was stopped
    HandshakeFailed, ///< The first frame wasn't an accepted Hello
    Broken,          ///< The pipe failed, a frame was too large or cut short, or a reply wasn't taken in time
    ActionDone,      ///< A Grant::OneAction session answered its one action
};

/// Answers requests on @p pipe, already accepted (BrokerPipe::acceptClient), until the client closes
/// it, nothing that putsOffTheWindowsIdleExit() accepts arrives for @p idleTimeout (reads come on their own timers, so
/// they don't put it off), or @p stopToken is stopped. The first frame must be a Hello of this protocol
/// version; until then nothing reaches @p handler. A frame that doesn't decode gets an ErrorReply, and
/// the session goes on; one declared larger than the protocol allows ends it, since a byte stream can't
/// skip it. The idle timeout runs on @p clock, steady_clock unless a test steps it (#2546).
/// Under Grant::OneAction (#2550), the session ends once the reply to its first request that is neither
/// the Hello nor isRead() is sent, whatever its result: any action, including one Windows refuses, so a
/// kind of action added later is limited too. Only the opening Hello puts off the idle exit, so a
/// client can't hold the consent open by greeting again.
[[nodiscard]] SessionEnd serve(HANDLE pipe,
                               const RequestHandler& handler,
                               std::chrono::milliseconds idleTimeout,
                               const std::stop_token& stopToken,
                               const Broker::IdleClock& clock = {},
                               Grant grant = Grant::Session);

} // namespace Platform::Windows::BrokerPipeServer

#endif // _WIN32
