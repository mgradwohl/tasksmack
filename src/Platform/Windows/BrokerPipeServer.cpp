#include "BrokerPipeServer.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/Windows/BrokerPipe.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace Platform::Windows::BrokerPipeServer
{

namespace
{

/// @p handler's answer, never an exception: one is an ErrorReply, and the session goes on.
[[nodiscard]] Broker::Response answer(const RequestHandler& handler, const Broker::Request& request)
{
    try
    {
        return handler(request);
    }
    catch (const std::exception& e)
    {
        return Broker::ErrorReply{.message = e.what()};
    }
    catch (...)
    {
        return Broker::ErrorReply{.message = "The request failed"};
    }
}

} // namespace

bool isActionWindowsDoes(const Broker::Request& request) noexcept
{
    // Every signal: Terminate, Kill, and since #2582 Suspend and Resume (NtSuspendProcess, NtResumeProcess);
    // Efficiency mode (#3071); Replace Task Manager (#3072), which isn't a process action but is one the
    // user chose, so it is the one action of an over-the-shoulder session too. The Users tab's Disconnect and
    // Sign out of another user's session (#3076), and one thread suspended, resumed or ended (#3069), are
    // actions the user chose and confirmed as well. An I/O priority hint (#3362): High needs an administrator.
    // Close connection (#3358): SetTcpEntry needs an administrator for any connection.
    return std::holds_alternative<Broker::SignalRequest>(request) || std::holds_alternative<Broker::PriorityRequest>(request) ||
           std::holds_alternative<Broker::IoPriorityRequest>(request) || std::holds_alternative<Broker::AffinityRequest>(request) ||
           std::holds_alternative<Broker::EfficiencyModeRequest>(request) ||
           std::holds_alternative<Broker::TaskManagerReplacementRequest>(request) ||
           std::holds_alternative<Broker::SessionActionRequest>(request) || std::holds_alternative<Broker::ThreadActionRequest>(request) ||
           std::holds_alternative<Broker::CloseConnectionRequest>(request);
}

bool isRead(const Broker::Request& request) noexcept
{
    // Every kind is sorted into the Hello, an action or a read. One added to Broker::Request must be
    // sorted here too, or a one-action session (#2550) and the over-the-shoulder read filter
    // (BrokerHelper::readsOnlyOwnProcesses) might let it through: revisit both, then this count.
    // TaskManagerReplacementRequest (#3072), SessionActionRequest (#3076), ThreadActionRequest (#3069) and
    // CloseConnectionRequest (#3358) are actions (isActionWindowsDoes()). Protocol v23 dropped the kinds only
    // Linux's helper served (#3580). OpenFilesBatchRequest (#3578) is a read: Find handles' open files.
    static_assert(std::variant_size_v<Broker::Request> == 16, "sort the new request kind into an action or a read");
    return std::holds_alternative<Broker::EnvironmentRequest>(request) || std::holds_alternative<Broker::ModulesRequest>(request) ||
           std::holds_alternative<Broker::OpenFilesRequest>(request) || std::holds_alternative<Broker::ConnectionsRequest>(request) ||
           std::holds_alternative<Broker::ThreadsRequest>(request) ||      // Process Details' threads (#3069)
           std::holds_alternative<Broker::OpenFilesBatchRequest>(request); // Find handles' administrator pass (#3578)
}

bool putsOffTheWindowsIdleExit(const Broker::Request& request) noexcept
{
    return Broker::putsOffTheIdleExit(request) && (std::holds_alternative<Broker::Hello>(request) || isActionWindowsDoes(request));
}

SessionEnd serve(HANDLE pipe,
                 const RequestHandler& handler,
                 std::chrono::milliseconds idleTimeout,
                 const std::stop_token& stopToken,
                 const Broker::IdleClock& clock,
                 Grant grant)
{
    bool greeted = false;
    // The idle timeout runs from the last request the user made: reads come on their own timers, so they
    // don't keep the helper alive (#2369, #2405).
    auto lastActivity = Broker::idleNow(clock);
    while (true)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(idleTimeout - (Broker::idleNow(clock) - lastActivity));
        if (left.count() <= 0)
        {
            return SessionEnd::Idle;
        }
        const auto frame = BrokerPipe::receiveFrame(pipe, left, stopToken);
        if (!frame.has_value())
        {
            switch (frame.error())
            {
            case BrokerPipe::ReceiveEnd::Closed:
                return SessionEnd::ClientClosed;
            case BrokerPipe::ReceiveEnd::TimedOut:
                return SessionEnd::Idle;
            case BrokerPipe::ReceiveEnd::Stopped:
                return SessionEnd::Stopped;
            case BrokerPipe::ReceiveEnd::Failed:
                break;
            }
            return SessionEnd::Broken;
        }
        const auto decoded = Broker::decodeRequest(*frame);
        Broker::Response response;
        std::uint32_t requestId = 0;
        bool endAfterReply = false;
        bool actionAnswered = false;
        if (!decoded.has_value())
        {
            response = Broker::ErrorReply{.message = decoded.error()};
            endAfterReply = !greeted; // a session that doesn't start with a Hello isn't one
        }
        else
        {
            requestId = decoded->requestId;
            const bool greetsAgain = greeted && std::holds_alternative<Broker::Hello>(decoded->message);
            if (putsOffTheWindowsIdleExit(decoded->message) && (grant != Grant::OneAction || !greetsAgain))
            {
                lastActivity = Broker::idleNow(clock);
            }
            if (const auto* hello = std::get_if<Broker::Hello>(&decoded->message))
            {
                const bool accepted = hello->version == Broker::PROTOCOL_VERSION;
                response = Broker::HelloReply{.version = Broker::PROTOCOL_VERSION, .accepted = accepted};
                greeted = accepted;
                endAfterReply = !accepted;
            }
            else if (!greeted)
            {
                response = Broker::ErrorReply{.message = "The session must start with a Hello"};
                endAfterReply = true;
            }
            else
            {
                response = answer(handler, decoded->message);
                // Fails closed (#2550): anything that isn't a read counts as the one action.
                actionAnswered = !isRead(decoded->message);
            }
        }
        std::vector<std::uint8_t> reply = Broker::encodeResponse(requestId, response);
        if (reply.size() > Broker::MAX_MESSAGE_BYTES)
        {
            // A reply too large for a frame fails that one request, not the session (#2403).
            reply = Broker::encodeResponse(requestId, Broker::ErrorReply{.message = "The helper's answer was too large to send"});
        }
        // Bounded, as every wait here is: a client that stops reading mid-reply is gone as far as this
        // session is concerned.
        std::string error;
        if (!BrokerPipe::sendFrame(pipe, reply, REPLY_TIMEOUT, stopToken, error))
        {
            return stopToken.stop_requested() ? SessionEnd::Stopped : SessionEnd::Broken;
        }
        if (endAfterReply)
        {
            return SessionEnd::HandshakeFailed;
        }
        if (grant == Grant::OneAction && actionAnswered)
        {
            return SessionEnd::ActionDone; // the consent was for this one action (#2550)
        }
    }
}

} // namespace Platform::Windows::BrokerPipeServer
