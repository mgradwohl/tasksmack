#pragma once

// The client side of the privileged broker (#2306; #2181): a connected elevated helper that does one
// BrokerProtocol request at a time. Each platform's transport implements it, a socketpair to a
// pkexec-launched helper on Linux (#2307) and a named pipe on Windows (#2311), so the decorators that
// retry a refused action or read through it (#2308, #2309) are shared. App owns one and hands it to the
// factories at construction; nothing makes one per frame.

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"

#include <chrono>
#include <cstdint>
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
    /// Never waits, but looks at the channel: it peeks at the pipe or polls the socket, and lets go of a
    /// helper that has gone. For a worker, or a call made once; the UI thread's per-frame reads use
    /// lastSeenConnected().
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    /// What connected() last found, or connect() last made it (#3182): one atomic read, with no system
    /// call and no lock, for the UI thread, every frame if need be. A helper that has just exited still
    /// reads as connected until a worker's connected() or call finds it gone, as the sampler's reads
    /// and the actions' retries do.
    [[nodiscard]] virtual bool lastSeenConnected() const noexcept = 0;

    /// Starts the helper if it isn't running, asking for the administrator's password first, and does
    /// the handshake (#2308). Blocking for as long as the prompt is up: call it from a worker, never
    /// the UI thread. The error says why there is no helper (the prompt was dismissed or refused, the
    /// helper isn't installed, it didn't answer).
    [[nodiscard]] virtual std::expected<void, std::string> connect(const std::stop_token& stopToken) = 0;

    /// "Authenticate for this session" (#2728): starts the helper if it isn't running (one prompt, so
    /// blocking like connect(): a worker, never the UI thread) and keeps it for the rest of TaskSmack's
    /// session instead of letting it exit when idle. A helper started later, after this one ended, is
    /// held too. A refused or dismissed prompt leaves the session as it was: the error says why. The
    /// default is for transports whose Administrator is something else (Windows restarts elevated).
    [[nodiscard]] virtual std::expected<void, std::string> holdForSession(const std::stop_token& stopToken)
    {
        static_cast<void>(stopToken);
        return std::unexpected(std::string("this platform's Administrator session doesn't use the helper"));
    }

    /// Whether the session is Administrator through the helper (#2728): holdForSession() succeeded and
    /// the helper it holds is still connected, as last seen. Cheap and non-blocking, for the UI thread:
    /// it makes no system call, so a helper that has just exited reads as held until a call finds out.
    [[nodiscard]] virtual bool heldForSession() const noexcept
    {
        return false;
    }

    /// Whether the helper, once started, can read processes that aren't TaskSmack's user's own (#2550):
    /// what "Authenticate to read" is for. False where every elevation runs as another account and the
    /// helper then reads only the user's own processes (a standard user's Windows TaskSmack, whose UAC
    /// prompt asks for an administrator's password): the readers then don't offer a prompt that can't
    /// help. Actions are offered either way. Known before the helper starts, and it can turn false
    /// after: on Windows, once a helper that was expected to run as the user answers a read saying it
    /// doesn't (#2769), until a new helper starts (#2993). Ask each time, don't cache it.
    [[nodiscard]] virtual bool readsOtherUsers() const noexcept
    {
        return true;
    }

    /// How many helper sessions connect() has started (#2993): one more each time it starts a helper and
    /// that helper answers the handshake, so an answer can be told from a later helper's, which may run
    /// as another account (each UAC prompt is answered afresh). Cheap and non-blocking, for the UI
    /// thread. The default, 0 for good, is for transports that don't count them.
    [[nodiscard]] virtual std::uint64_t sessionsStarted() const noexcept
    {
        return 0;
    }

    /// Sends @p request and waits for its response, for at most the transport's bounded wait or until
    /// @p stopToken is stopped. Blocking: call it from a worker (ProcessActionRun, #2305), never the
    /// UI thread. The error is why there is no response (not connected, broken channel, timed out).
    [[nodiscard]] virtual std::expected<Broker::Response, std::string> call(const Broker::Request& request,
                                                                            const std::stop_token& stopToken) = 0;

    /// call() for a thread that must not stall, such as the process sampler's PSS/USS read (#2369): never
    /// waits for another call to finish (the error is "busy" instead), and waits at most @p timeout for
    /// its own response. A response that takes longer skips this read, not the session (#2406). Transports without contention keep this
    /// default, which is call().
    [[nodiscard]] virtual std::expected<Broker::Response, std::string> callIfIdle(const Broker::Request& request,
                                                                                  std::chrono::milliseconds timeout)
    {
        static_cast<void>(timeout);
        return call(request, std::stop_token{});
    }
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
