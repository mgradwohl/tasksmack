#pragma once

// The session rules every privileged-broker transport's server shares (#2307, #2311), so the Linux
// socket helper and the Windows pipe helper end their sessions alike. Not part of the wire format:
// BrokerProtocol.h says what a frame carries, this says what the helper does with the session.

#include "Platform/BrokerProtocol.h"

#include <chrono>
#include <functional>
#include <variant>

namespace Platform::Broker
{

/// Whether @p request puts off the helper's idle exit: only what the user does, the Hello (which an
/// Authenticate and retry or Authenticate to read connects with) and an action. Every read kind can
/// come on its own: Process Details re-reads Environment, Modules, Open files and Connections through
/// a connected helper on its refresh timer, and the sampler reads PSS/USS every refresh (#2369, #2405).
[[nodiscard]] inline bool putsOffTheIdleExit(const Request& request) noexcept
{
    return std::holds_alternative<Hello>(request) || std::holds_alternative<SignalRequest>(request) ||
           std::holds_alternative<PriorityRequest>(request) || std::holds_alternative<IoPriorityRequest>(request) ||
           std::holds_alternative<AffinityRequest>(request) || std::holds_alternative<ServiceActionRequest>(request);
}

/// Where a server's idle timeout reads the time: an empty one is steady_clock. Tests pass a clock they
/// step from the handler, so whether a session ends idle doesn't hang on how fast the machine is (#2546).
using IdleClock = std::function<std::chrono::steady_clock::time_point()>;

/// The time on @p clock, or steady_clock's when it is empty.
[[nodiscard]] inline std::chrono::steady_clock::time_point idleNow(const IdleClock& clock)
{
    return clock ? clock() : std::chrono::steady_clock::now();
}

} // namespace Platform::Broker
