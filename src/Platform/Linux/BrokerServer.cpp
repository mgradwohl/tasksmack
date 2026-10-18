#include "BrokerServer.h"

#include "BrokerSocket.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"

#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <stop_token>
#include <string>
#include <utility>
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

/// Whether @p request puts off the idle exit: only what the user does, the Hello (which an
/// Authenticate and retry or Authenticate to read connects with) and an action. Every read kind can
/// come on its own: Process Details re-reads Environment, Modules, Open files and Connections through
/// a connected helper on its refresh timer, and the sampler reads PSS/USS every refresh (#2369, #2405).
[[nodiscard]] bool putsOffTheIdleExit(const Broker::Request& request) noexcept
{
    return std::holds_alternative<Broker::Hello>(request) || std::holds_alternative<Broker::SignalRequest>(request) ||
           std::holds_alternative<Broker::PriorityRequest>(request) || std::holds_alternative<Broker::IoPriorityRequest>(request) ||
           std::holds_alternative<Broker::AffinityRequest>(request);
}

} // namespace

Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, const Readers& readers)
{
    if (const auto* hello = std::get_if<Broker::Hello>(&request))
    {
        return Broker::HelloReply{.version = Broker::PROTOCOL_VERSION, .accepted = hello->version == Broker::PROTOCOL_VERSION};
    }
    // Process Details' reads (#2309): each reader checks the target's identity itself. The environment
    // reader never logs a value (#179).
    if (const auto* read = std::get_if<Broker::EnvironmentRequest>(&request))
    {
        if (readers.environment == nullptr)
        {
            return Broker::ErrorReply{.message = "This helper doesn't read environments"};
        }
        try
        {
            return Broker::EnvironmentReply{.result = readers.environment->readEnvironment(read->target)};
        }
        catch (...)
        {
            return Broker::EnvironmentReply{.result = {.status = EnvironmentReadStatus::Failed, .variables = {}}};
        }
    }
    if (const auto* read = std::get_if<Broker::ModulesRequest>(&request))
    {
        if (readers.modules == nullptr)
        {
            return Broker::ErrorReply{.message = "This helper doesn't list modules"};
        }
        try
        {
            return Broker::ModulesReply{.result = readers.modules->readModules(read->target)};
        }
        catch (...)
        {
            return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = {}}};
        }
    }
    if (const auto* read = std::get_if<Broker::OpenFilesRequest>(&request))
    {
        if (readers.openFiles == nullptr)
        {
            return Broker::ErrorReply{.message = "This helper doesn't list open files"};
        }
        try
        {
            return Broker::OpenFilesReply{.result = readers.openFiles->readOpenFiles(read->target)};
        }
        catch (...)
        {
            OpenFilesReadResult failed;
            failed.status = OpenFilesReadStatus::Failed;
            return Broker::OpenFilesReply{.result = std::move(failed)};
        }
    }
    if (const auto* read = std::get_if<Broker::ConnectionsRequest>(&request))
    {
        if (readers.connections == nullptr)
        {
            return Broker::ErrorReply{.message = "This helper doesn't list connections"};
        }
        try
        {
            return Broker::ConnectionsReply{.result = readers.connections->readConnections(read->target)};
        }
        catch (...)
        {
            return Broker::ConnectionsReply{.result = {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = {}}};
        }
    }
    if (const auto* read = std::get_if<Broker::MemoryRollupRequest>(&request))
    {
        if (!readers.memoryRollup)
        {
            return Broker::ErrorReply{.message = "This helper doesn't read PSS and USS"};
        }
        try
        {
            return Broker::MemoryRollupReply{.result = readers.memoryRollup(read->target.pid, read->target.startTimeTicks)};
        }
        catch (...)
        {
            return Broker::MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
        }
    }
    if (const auto* read = std::get_if<Broker::FirmwareFileRequest>(&request))
    {
        if (!readers.firmwareFile)
        {
            return Broker::ErrorReply{.message = "This helper doesn't read firmware files"};
        }
        try
        {
            return readers.firmwareFile(read->file);
        }
        catch (...)
        {
            return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
        }
    }
    if (std::holds_alternative<Broker::CrashesRequest>(request))
    {
        if (!readers.crashes)
        {
            return Broker::ErrorReply{.message = "This helper doesn't read crashes"};
        }
        try
        {
            return Broker::CrashesReply{.info = readers.crashes()};
        }
        catch (...)
        {
            return Broker::ErrorReply{.message = "The crash read failed"};
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

SessionEnd
serve(int fd, IProcessActions& actions, std::chrono::milliseconds idleTimeout, const std::stop_token& stopToken, const Readers& readers)
{
    bool greeted = false;
    // The idle timeout runs from the last request the user made: reads come on their own timers, so they
    // don't keep the helper alive (#2369, #2405).
    auto lastActivity = std::chrono::steady_clock::now();
    while (true)
    {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(idleTimeout - (std::chrono::steady_clock::now() - lastActivity));
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
            if (putsOffTheIdleExit(decoded->message))
            {
                lastActivity = std::chrono::steady_clock::now();
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
