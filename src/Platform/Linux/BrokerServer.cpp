#include "BrokerServer.h"

#include "BrokerSocket.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerRequestMapping.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IProcessActions.h"

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace Platform::BrokerServer
{

namespace
{
/// How long a held session waits for a frame before it looks again: any length works, as a held session
/// never ends idle; this one keeps the wait's arithmetic far from overflowing.
constexpr std::chrono::milliseconds HELD_WAIT = std::chrono::hours{1};
} // namespace

SessionEnd serve(int fd,
                 IProcessActions& actions,
                 std::chrono::milliseconds idleTimeout,
                 const std::stop_token& stopToken,
                 const Readers& readers,
                 const Broker::IdleClock& clock)
{
    bool greeted = false;
    // Held for the session (#2728): a Hello asked for it, and the session then ends only when TaskSmack
    // closes the socket, the session is stopped or the channel breaks.
    bool held = false;
    // The idle timeout runs from the last request the user made: reads come on their own timers, so they
    // don't keep the helper alive (#2369, #2405).
    auto lastActivity = Broker::idleNow(clock);
    while (true)
    {
        const auto left =
            held ? HELD_WAIT : std::chrono::duration_cast<std::chrono::milliseconds>(idleTimeout - (Broker::idleNow(clock) - lastActivity));
        if (left.count() <= 0)
        {
            return SessionEnd::Idle;
        }
        const auto frame = BrokerSocket::receiveFrame(fd, left, stopToken);
        if (!frame.has_value())
        {
            switch (frame.error())
            {
            case BrokerSocket::ReceiveEnd::Closed:
                return SessionEnd::ClientClosed;
            case BrokerSocket::ReceiveEnd::TimedOut:
                if (held)
                {
                    continue;
                }
                return SessionEnd::Idle;
            case BrokerSocket::ReceiveEnd::Stopped:
                return SessionEnd::Stopped;
            case BrokerSocket::ReceiveEnd::Failed:
                return SessionEnd::Broken;
            }
        }
        const auto decoded = Broker::decodeRequest(*frame);
        Broker::Response response;
        std::uint32_t requestId = 0;
        bool endAfterReply = false;
        if (!decoded.has_value())
        {
            response = Broker::ErrorReply{.message = decoded.error()};
            endAfterReply = !greeted; // a session that doesn't start with a Hello isn't one
        }
        else
        {
            requestId = decoded->requestId;
            if (Broker::putsOffTheIdleExit(decoded->message))
            {
                lastActivity = Broker::idleNow(clock);
            }
            const bool hello = std::holds_alternative<Broker::Hello>(decoded->message);
            if (!greeted && !hello)
            {
                response = Broker::ErrorReply{.message = "The session must start with a Hello"};
                endAfterReply = true;
            }
            else
            {
                response = handleRequest(decoded->message, actions, readers);
                if (hello)
                {
                    greeted = std::get<Broker::HelloReply>(response).accepted;
                    endAfterReply = !greeted;
                    // Once held, held: a later Hello without the flag doesn't put the idle exit back.
                    held = held || (greeted && std::get<Broker::Hello>(decoded->message).forSession);
                }
            }
        }
        std::vector<std::uint8_t> reply = Broker::encodeResponse(requestId, response);
        if (reply.size() > Broker::MAX_MESSAGE_BYTES)
        {
            // A reply too large for a frame fails that one read, not the session: sendFrame() would
            // refuse it, and ending the session over it costs every other section its authorization
            // (#2403). The list replies stop short of the limit themselves; this catches the rest.
            reply = Broker::encodeResponse(requestId, Broker::ErrorReply{.message = "The helper's answer was too large to send"});
        }
        std::string error;
        if (!BrokerSocket::sendFrame(fd, reply, error))
        {
            return SessionEnd::Broken;
        }
        if (endAfterReply)
        {
            return SessionEnd::HandshakeFailed;
        }
    }
}

} // namespace Platform::BrokerServer
