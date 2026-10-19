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

/// How a session ended, for the helper's exit status and the tests.
enum class SessionEnd : std::uint8_t
{
    ClientClosed,    ///< The client closed its end (or exited): the normal end
    Idle,            ///< No Hello or action within the idle timeout
    Stopped,         ///< The stop token was stopped
    HandshakeFailed, ///< The first frame wasn't an accepted Hello
    Broken,          ///< The pipe failed, a frame was too large or cut short, or a reply wasn't taken in time
};

/// Answers requests on @p pipe, already accepted (BrokerPipe::acceptClient), until the client closes
/// it, no Hello or action arrives for @p idleTimeout (reads come on their own timers, so they don't put
/// it off), or @p stopToken is stopped. The first frame must be a Hello of this protocol version; until
/// then nothing reaches @p handler. A frame that doesn't decode gets an ErrorReply, and the session goes
/// on; one declared larger than the protocol allows ends it, since a byte stream can't skip it. The idle
/// timeout runs on @p clock, steady_clock unless a test steps it (#2546).
[[nodiscard]] SessionEnd serve(HANDLE pipe,
                               const RequestHandler& handler,
                               std::chrono::milliseconds idleTimeout,
                               const std::stop_token& stopToken,
                               const Broker::IdleClock& clock = {});

} // namespace Platform::Windows::BrokerPipeServer

#endif // _WIN32
