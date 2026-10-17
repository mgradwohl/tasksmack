#include "BrokerServer.h"

#include "BrokerSocket.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <stop_token>
#include <string>
#include <variant>

namespace Platform::BrokerServer
{
namespace
{

[[nodiscard]] Broker::ActionResult act(const Broker::Request& request, IProcessActions& actions)
{
    if (const auto* signal = std::get_if<Broker::SignalRequest>(&request))
    {
        switch (signal->signal)
        {
        case Broker::Signal::Terminate:
            return Broker::ActionResult::from(actions.terminate(signal->target));
        case Broker::Signal::Kill:
            return Broker::ActionResult::from(actions.kill(signal->target));
        case Broker::Signal::Suspend:
            return Broker::ActionResult::from(actions.stop(signal->target));
        case Broker::Signal::Resume:
            return Broker::ActionResult::from(actions.resume(signal->target));
        }
    }
    if (const auto* priority = std::get_if<Broker::PriorityRequest>(&request))
    {
        return Broker::ActionResult::from(actions.setPriority(priority->target, priority->nice));
    }
    if (const auto* io = std::get_if<Broker::IoPriorityRequest>(&request))
    {
        return Broker::ActionResult::from(actions.setIoPriority(io->target, io->ioClass, io->level));
    }
    if (const auto* affinity = std::get_if<Broker::AffinityRequest>(&request))
    {
        return Broker::ActionResult::from(actions.setAffinity(affinity->target, affinity->cpus));
    }
    return {.success = false, .errorMessage = "Not an action", .successNote = {}};
}

} // namespace

Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, IProcessEnvironmentReader* environment)
{
    if (const auto* hello = std::get_if<Broker::Hello>(&request))
    {
        return Broker::HelloReply{.version = Broker::PROTOCOL_VERSION, .accepted = hello->version == Broker::PROTOCOL_VERSION};
    }
    if (const auto* read = std::get_if<Broker::EnvironmentRequest>(&request))
    {
        if (environment == nullptr)
        {
            return Broker::ErrorReply{.message = "This helper doesn't read environments"};
        }
        try
        {
            // The reader checks the target's identity and never logs a value (#179).
            return Broker::EnvironmentReply{.result = environment->readEnvironment(read->target)};
        }
        catch (...)
        {
            return Broker::EnvironmentReply{.result = {.status = EnvironmentReadStatus::Failed, .variables = {}}};
        }
    }
    try
    {
        return act(request, actions);
    }
    catch (const std::exception& e)
    {
        return Broker::ActionResult{.success = false, .errorMessage = e.what(), .successNote = {}};
    }
    catch (...)
    {
        return Broker::ActionResult{.success = false, .errorMessage = "The action failed", .successNote = {}};
    }
}

SessionEnd serve(int fd,
                 IProcessActions& actions,
                 std::chrono::milliseconds idleTimeout,
                 const std::stop_token& stopToken,
                 IProcessEnvironmentReader* environment)
{
    bool greeted = false;
    while (true)
    {
        const auto frame = BrokerSocket::receiveFrame(fd, idleTimeout, stopToken);
        if (!frame.has_value())
        {
            switch (frame.error())
            {
            case BrokerSocket::ReceiveEnd::Closed:
                return SessionEnd::ClientClosed;
            case BrokerSocket::ReceiveEnd::TimedOut:
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
            const bool hello = std::holds_alternative<Broker::Hello>(decoded->message);
            if (!greeted && !hello)
            {
                response = Broker::ErrorReply{.message = "The session must start with a Hello"};
                endAfterReply = true;
            }
            else
            {
                response = handleRequest(decoded->message, actions, environment);
                if (hello)
                {
                    greeted = std::get<Broker::HelloReply>(response).accepted;
                    endAfterReply = !greeted;
                }
            }
        }
        std::string error;
        if (!BrokerSocket::sendFrame(fd, Broker::encodeResponse(requestId, response), error))
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
