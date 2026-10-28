#include "BrokerRequestMapping.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IServiceActions.h"

#include <exception>
#include <expected>
#include <optional>
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
    if (const auto* efficiency = std::get_if<Broker::EfficiencyModeRequest>(&request))
    {
        return Broker::ActionResult::from(actions.setEfficiencyMode(efficiency->target, efficiency->on));
    }
    return {.success = false, .errorMessage = "Not an action", .successNote = {}};
}

} // namespace

namespace
{

// One handler per read kind (#2514). Each runs with the helper's privilege (root on Linux, elevated on
// Windows) for the TaskSmack that started it, so each says what it allows. A reader that throws gives
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

/// The Memory section's PSS and USS (#2309): the read checks the target's start time before trusting
/// what it reads (smaps_rollup on Linux).
[[nodiscard]] Broker::Response handleMemoryRollup(const Broker::MemoryRollupRequest& read, const Readers& readers)
{
    if (!readers.memoryRollup)
    {
        return Broker::ErrorReply{.message = "This helper doesn't read PSS and USS"};
    }
    try
    {
        return Broker::MemoryRollupReply{.result = readers.memoryRollup(read.target.pid, read.target.startTimeTicks)};
    }
    catch (...)
    {
        return Broker::MemoryRollupReply{.result = std::unexpected(MemoryRollupFailure::Unavailable)};
    }
}

/// Many processes' FD counts and I/O bytes (#2728): an entry for each target its reader still finds, in order.
[[nodiscard]] Broker::Response handleProcessCounters(const Broker::ProcessCountersRequest& read, const Readers& readers)
{
    if (!readers.processCounters)
    {
        return Broker::ErrorReply{.message = "This helper doesn't read process counters"};
    }
    Broker::ProcessCountersReply reply;
    reply.entries.reserve(read.targets.size());
    for (const ProcessTarget& target : read.targets)
    {
        try
        {
            if (std::optional<Broker::ProcessCountersEntry> entry = readers.processCounters(target, read.withSockets))
            {
                reply.entries.push_back(std::move(*entry));
            }
        }
        catch (...)
        {
            continue; // that one process is left out; the others' counters still go back
        }
    }
    return reply;
}

/// The processes hidepid hides from TaskSmack (#2728). A helper without the reader answers with an error,
/// which TaskSmack takes as "this helper never lists them" and doesn't ask again. A reader that throws
/// lists none this time, marked truncated, so the next refresh asks again (#3032): one failed read, a
/// process gone mid-walk or a failed allocation, isn't the helper lacking the reader.
[[nodiscard]] Broker::Response handleHiddenProcesses(const Broker::HiddenProcessesRequest& read, const Readers& readers)
{
    if (!readers.hiddenProcesses)
    {
        return Broker::ErrorReply{.message = "This helper doesn't list hidden processes"};
    }
    try
    {
        return readers.hiddenProcesses(read.visiblePids);
    }
    catch (...)
    {
        return Broker::HiddenProcessesReply{.processes = {}, .truncated = true};
    }
}

/// A service action (#2728): a helper without the reader (Windows') answers with an error.
[[nodiscard]] Broker::Response handleServiceAction(const Broker::ServiceActionRequest& request, const Readers& readers)
{
    if (!readers.serviceAction)
    {
        return Broker::ErrorReply{.message = "This helper doesn't act on services"};
    }
    try
    {
        return Broker::ServiceActionReply{.result = readers.serviceAction(request)};
    }
    catch (const std::exception& error)
    {
        return Broker::ServiceActionReply{.result = ServiceActionResult::failed(std::string("The service action failed: ") + error.what())};
    }
}

/// System Information's privileged firmware facts (#2310): only the files FirmwareFile names, never a path.
[[nodiscard]] Broker::Response handleFirmwareFile(const Broker::FirmwareFileRequest& read, const Readers& readers)
{
    if (!readers.firmwareFile)
    {
        return Broker::ErrorReply{.message = "This helper doesn't read firmware files"};
    }
    try
    {
        return readers.firmwareFile(read.file);
    }
    catch (...)
    {
        return Broker::FirmwareFileReply{.status = Broker::FirmwareFileStatus::Unreadable, .bytes = {}};
    }
}

/// Every user's crashes (#2310). On Linux: journal entries the reader trust-checks, and cores from the
/// coredump directory only.
[[nodiscard]] Broker::Response handleCrashes(const Readers& readers)
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
    if (const auto* read = std::get_if<Broker::ConnectionsRequest>(&request))
    {
        return handleConnections(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::MemoryRollupRequest>(&request))
    {
        return handleMemoryRollup(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::FirmwareFileRequest>(&request))
    {
        return handleFirmwareFile(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::ProcessCountersRequest>(&request))
    {
        return handleProcessCounters(*read, readers);
    }
    if (const auto* read = std::get_if<Broker::HiddenProcessesRequest>(&request))
    {
        return handleHiddenProcesses(*read, readers);
    }
    if (const auto* service = std::get_if<Broker::ServiceActionRequest>(&request))
    {
        return handleServiceAction(*service, readers);
    }
    if (std::holds_alternative<Broker::CrashesRequest>(request))
    {
        return handleCrashes(readers);
    }
    return handleAction(request, actions);
}

} // namespace Platform::BrokerServer
