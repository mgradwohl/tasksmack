#pragma once

// The privileged helper's side of the broker (#2307): reads BrokerProtocol requests from its client
// over a BrokerSocket and answers each through LinuxProcessControl (#2640), which (running as root)
// re-verifies every target by PID and start time before it acts. handleRequest() (Platform/BrokerRequestMapping.h,
// shared with the Windows helper) is the pure mapping; serve() is the loop tasksmack-helper runs.

#include "Platform/BrokerRequestMapping.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IProcessActions.h"

#include <chrono>
#include <cstdint>
#include <stop_token>

namespace Platform::BrokerServer
{

/// How a session ended, for the helper's exit status and the tests.
enum class SessionEnd : std::uint8_t
{
    ClientClosed,    ///< The client closed its end (or exited): the normal end
    Idle,            ///< No Hello or action within the idle timeout
    Stopped,         ///< The stop token was stopped
    HandshakeFailed, ///< The first frame wasn't an accepted Hello
    Broken,          ///< The socket failed
};

/// Answers requests on @p fd until the client closes it, no Hello or action arrives for @p idleTimeout
/// (reads come on their own timers, so they don't put it off; a Hello with forSession turns this off for
/// the rest of the session, #2728), or @p stopToken is stopped. The first
/// frame must be a Hello of this protocol version; until then nothing else is acted on. A frame that
/// doesn't decode gets an ErrorReply, and the session goes on. The idle timeout runs on @p clock,
/// steady_clock unless a test steps it (#2546).
[[nodiscard]] SessionEnd serve(int fd,
                               IProcessActions& actions,
                               std::chrono::milliseconds idleTimeout,
                               const std::stop_token& stopToken,
                               const Readers& readers = {},
                               const Broker::IdleClock& clock = {});

} // namespace Platform::BrokerServer
