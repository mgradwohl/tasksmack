#include "BrokerRequestMapping.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/BrokerSessionRules.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IServiceActions.h"

#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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
        case Broker::Signal::Hangup:
            return Broker::ActionResult::from(actions.sendSignal(signal->target, ProcessSignal::Hangup));
        case Broker::Signal::Interrupt:
            return Broker::ActionResult::from(actions.sendSignal(signal->target, ProcessSignal::Interrupt));
        case Broker::Signal::Quit:
            return Broker::ActionResult::from(actions.sendSignal(signal->target, ProcessSignal::Quit));
        case Broker::Signal::User1:
            return Broker::ActionResult::from(actions.sendSignal(signal->target, ProcessSignal::User1));
        case Broker::Signal::User2:
            return Broker::ActionResult::from(actions.sendSignal(signal->target, ProcessSignal::User2));
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
    // By its inode (Linux, #3066), or with none, by its IPv4 TCP endpoints (Windows, #3358).
    if (const auto* close = std::get_if<Broker::CloseConnectionRequest>(&request))
    {
        if (close->socketInode == 0 && close->tcpConnection != TcpConnectionToClose{})
        {
            return Broker::ActionResult::from(actions.closeTcpConnection(close->target, close->tcpConnection));
        }
        return Broker::ActionResult::from(actions.closeConnection(close->target, close->socketInode));
    }
    // An off with the class TaskSmack remembers the on replaced (#3327): set here first, replacing any this helper
    // kept from an earlier on, so the off restores it though this helper never saw the on. The on's reply carries the
    // class it replaced back.
    if (const auto* efficiency = std::get_if<Broker::EfficiencyModeRequest>(&request))
    {
        if (!efficiency->on && efficiency->restorePriorityClass != 0)
        {
            actions.setEfficiencyPriorityClassToRestore(efficiency->target, efficiency->restorePriorityClass);
        }
        return Broker::ActionResult::from(actions.setEfficiencyMode(efficiency->target, efficiency->on));
    }
    // One thread suspended, resumed or ended (#3069): the actions check the thread is the target's.
    if (const auto* thread = std::get_if<Broker::ThreadActionRequest>(&request))
    {
        return Broker::ActionResult::from(actions.actOnThread(thread->target, thread->tid, thread->threadCreateTime, thread->action));
    }
    return {.success = false, .errorMessage = "Not an action", .successNote = {}};
}

} // namespace

namespace
{

// One handler per read kind (#2514). Each runs with the helper's privilege (elevated, on Windows) for the
// TaskSmack that started it, so each says what it allows. A reader that throws gives
// its kind's failure, never an escaped exception.

/// Process Details' environment (#2309): the reader checks the target's identity (PID and start time)
/// itself and never logs a value (#179).
[[nodiscard]] Broker::Response handleEnvironment(const Broker::EnvironmentRequest& read, const Readers& readers)
{
    if (readers.environment == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't read environments"};
    }
    try
    {
        return Broker::EnvironmentReply{.result = readers.environment->readEnvironment(read.target)};
    }
    catch (...)
    {
        return Broker::EnvironmentReply{.result = {.status = EnvironmentReadStatus::Failed, .variables = {}}};
    }
}

/// Process Details' modules (#2309): the reader checks the target's identity itself.
[[nodiscard]] Broker::Response handleModules(const Broker::ModulesRequest& read, const Readers& readers)
{
    if (readers.modules == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list modules"};
    }
    try
    {
        return Broker::ModulesReply{.result = readers.modules->readModules(read.target)};
    }
    catch (...)
    {
        return Broker::ModulesReply{.result = {.status = ModulesReadStatus::Failed, .modules = {}, .detail = {}}};
    }
}

/// Process Details' open files (#2309): the reader checks the target's identity itself.
[[nodiscard]] Broker::Response handleOpenFiles(const Broker::OpenFilesRequest& read, const Readers& readers)
{
    if (readers.openFiles == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list open files"};
    }
    try
    {
        return Broker::OpenFilesReply{.result = readers.openFiles->readOpenFiles(read.target)};
    }
    catch (...)
    {
        OpenFilesReadResult failed;
        failed.status = OpenFilesReadStatus::Failed;
        return Broker::OpenFilesReply{.result = std::move(failed)};
    }
}

/// Find handles' administrator pass (#3578): every target read in one batch of the reader's (beginBatch(): on Windows,
/// one walk of the system handle table), in order. Stops once OPEN_FILES_BATCH_BUDGET has passed, after at least one
/// read, and answers with the results so far; the client asks again for the rest. A read that throws is that
/// target's failure, and the rest are still read.
[[nodiscard]] Broker::Response handleOpenFilesBatch(const Broker::OpenFilesBatchRequest& read, const Readers& readers)
{
    if (readers.openFiles == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list open files"};
    }
    struct BatchScope
    {
        explicit BatchScope(IProcessOpenFilesReader& reader) : m_Reader(reader)
        {
            m_Reader.beginBatch();
        }
        ~BatchScope()
        {
            m_Reader.endBatch();
        }
        BatchScope(const BatchScope&) = delete;
        BatchScope& operator=(const BatchScope&) = delete;
        BatchScope(BatchScope&&) = delete;
        BatchScope& operator=(BatchScope&&) = delete;

      private:
        IProcessOpenFilesReader& m_Reader;
    };
    try
    {
        const auto deadline = Broker::idleNow(readers.clock) + OPEN_FILES_BATCH_BUDGET;
        Broker::OpenFilesBatchReply reply;
        reply.results.reserve(read.targets.size());
        const BatchScope batch(*readers.openFiles);
        for (const ProcessTarget& target : read.targets)
        {
            if (!reply.results.empty() && Broker::idleNow(readers.clock) >= deadline)
            {
                break; // the rest in the client's next request
            }
            try
            {
                reply.results.push_back(readers.openFiles->readOpenFiles(target));
            }
            catch (...)
            {
                OpenFilesReadResult failed;
                failed.status = OpenFilesReadStatus::Failed;
                reply.results.push_back(std::move(failed));
            }
        }
        return reply;
    }
    catch (...) // the batch's walk, or no memory for the reply
    {
        return Broker::ErrorReply{.message = "The open files couldn't be read"};
    }
}

/// Process Details' threads (#3069): the reader checks the target's identity itself.
[[nodiscard]] Broker::Response handleThreads(const Broker::ThreadsRequest& read, const Readers& readers)
{
    if (readers.threads == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list threads"};
    }
    try
    {
        return Broker::ThreadsReply{.result = readers.threads->readThreads(read.target)};
    }
    catch (...)
    {
        return Broker::ThreadsReply{.result = {.status = ThreadsReadStatus::Failed, .threads = {}, .detail = {}}};
    }
}

/// Process Details' connections (#2309): the reader checks the target's identity itself.
[[nodiscard]] Broker::Response handleConnections(const Broker::ConnectionsRequest& read, const Readers& readers)
{
    if (readers.connections == nullptr)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list connections"};
    }
    try
    {
        return Broker::ConnectionsReply{.result = readers.connections->readConnections(read.target)};
    }
    catch (...)
    {
        return Broker::ConnectionsReply{.result = {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = {}}};
    }
}

/// Replace Task Manager (#3072): a helper without the handler answers with an error. The handler
/// checks the path itself; one that throws is a failed ActionResult.
[[nodiscard]] Broker::Response handleTaskManagerReplacement(const Broker::TaskManagerReplacementRequest& request, const Readers& readers)
{
    if (!readers.taskManagerReplacement)
    {
        return Broker::ErrorReply{.message = "This helper doesn't replace Task Manager"};
    }
    try
    {
        return readers.taskManagerReplacement(request);
    }
    catch (const std::exception& error)
    {
        return Broker::ActionResult{
            .success = false,
            .errorMessage = std::string("Replacing Task Manager failed: ") + error.what(),
            .successNote = {},
        };
    }
}

/// A session's Disconnect or Sign out (#3076): a helper without the handler answers with an error.
[[nodiscard]] Broker::Response handleSessionAction(const Broker::SessionActionRequest& request, const Readers& readers)
{
    if (!readers.sessionAction)
    {
        return Broker::ErrorReply{.message = "This helper doesn't act on sessions"};
    }
    try
    {
        return Broker::ServiceActionReply{.result = readers.sessionAction(request)};
    }
    catch (const std::exception& error)
    {
        return Broker::ServiceActionReply{.result = ServiceActionResult::failed(std::string("The session action failed: ") + error.what())};
    }
}

/// An action on a process (act()): the platform's actions re-verify the target (PID and start time)
/// before acting.
[[nodiscard]] Broker::Response handleAction(const Broker::Request& request, IProcessActions& actions)
{
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

} // namespace

/// The reply to @p request: the handshake, a read through its handler above, or an action (#2514).
Broker::Response handleRequest(const Broker::Request& request, IProcessActions& actions, const Readers& readers)
{
    if (const auto* hello = std::get_if<Broker::Hello>(&request))
    {
        return Broker::HelloReply{.version = Broker::PROTOCOL_VERSION, .accepted = hello->version == Broker::PROTOCOL_VERSION};
    }
    if (const auto* read = std::get_if<Broker::EnvironmentRequest>(&request))
    {
        return handleEnvironment(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::ModulesRequest>(&request))
    {
        return handleModules(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::OpenFilesRequest>(&request))
    {
        return handleOpenFiles(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::OpenFilesBatchRequest>(&request))
    {
        return handleOpenFilesBatch(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::ConnectionsRequest>(&request))
    {
        return handleConnections(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::ThreadsRequest>(&request))
    {
        return handleThreads(*read, readers);
    }
    if (const auto* replacement = std::get_if<Broker::TaskManagerReplacementRequest>(&request))
    {
        return handleTaskManagerReplacement(*replacement, readers);
    }
    if (const auto* session = std::get_if<Broker::SessionActionRequest>(&request))
    {
        return handleSessionAction(*session, readers);
    }
    return handleAction(request, actions);
}

} // namespace Platform::BrokerServer
