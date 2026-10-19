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
    if (const auto* signal = std::get_if<Broker::SignalRequest>(&request))
    {
        return signal->signal == Broker::Signal::Terminate || signal->signal == Broker::Signal::Kill;
    }
    return std::holds_alternative<Broker::PriorityRequest>(request) || std::holds_alternative<Broker::AffinityRequest>(request);
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
                actionAnswered = isActionWindowsDoes(decoded->message);
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
