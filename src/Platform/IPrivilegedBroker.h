#pragma once

// The client side of the privileged broker (#2306; #2181): a connected elevated helper that does one
// BrokerProtocol request at a time. Each platform's transport implements it, a socketpair to a
// pkexec-launched helper on Linux (#2307) and a named pipe on Windows (#2311), so the decorators that
// retry a refused action or read through it (#2308, #2309) are shared. App owns one and hands it to the
// factories at construction; nothing makes one per frame.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"

#include <expected>
#include <stop_token>
#include <string>
#include <variant>

namespace Platform
{

class IPrivilegedBroker
{
  public:
    virtual ~IPrivilegedBroker() = default;

    IPrivilegedBroker() = default;
    IPrivilegedBroker(const IPrivilegedBroker&) = delete;
    IPrivilegedBroker& operator=(const IPrivilegedBroker&) = delete;
    IPrivilegedBroker(IPrivilegedBroker&&) = delete;
    IPrivilegedBroker& operator=(IPrivilegedBroker&&) = delete;

    /// Whether the helper is running and answered the handshake. False after it exits or the channel
    /// breaks; callers then act in-process, and the UI offers to reconnect.
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    /// Sends @p request and waits for its response, for at most the transport's bounded wait or until
    /// @p stopToken is stopped. Blocking: call it from a worker (ProcessActionRun, #2305), never the
    /// UI thread. The error is why there is no response (not connected, broken channel, timed out).
    [[nodiscard]] virtual std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                            const std::stop_token& stopToken) = 0;
};

/// A process action's result from a broker call: its ActionResult, or a failure saying why there was
/// none (the transport's error, the helper's ErrorReply, or a response of the wrong kind).
[[nodiscard]] inline ProcessActionResult actionResultOf(const std::expected<Broker::Response, std::string>& response)
{
    if (!response.has_value())
    {
        return ProcessActionResult::error("The administrator helper couldn't be asked: " + response.error());
    }
    if (const auto* result = std::get_if<Broker::ActionResult>(&*response))
    {
        return result->toProcessActionResult();
    }
    if (const auto* error = std::get_if<Broker::ErrorReply>(&*response))
    {
        return ProcessActionResult::error("The administrator helper refused it: " + error->message);
    }
    return ProcessActionResult::error("The administrator helper gave an unexpected answer");
}

} // namespace Platform
