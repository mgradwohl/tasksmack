#pragma once

// The session rules every privileged-broker transport's server shares (#2307, #2311), so the Linux
// socket helper and the Windows pipe helper end their sessions alike. Not part of the wire format:
// BrokerProtocol.h says what a frame carries, this says what the helper does with the session.

#include "Platform/BrokerProtocol.h"

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
           std::holds_alternative<AffinityRequest>(request);
}

} // namespace Platform::Broker
