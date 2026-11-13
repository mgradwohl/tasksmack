#include "BrokeredProcessActions.h"

#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessMemoryMap.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IoPriorityTypes.h"
#include "Platform/SchedPolicy.h"
#include "Platform/WaitChain.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{

namespace
{

/// What runAuthorized() says of an action no broker request does (#3580).
constexpr std::string_view NOT_THROUGH_THE_HELPER = "Setting a scheduling policy isn't available through the administrator helper";

} // namespace

Broker::Signal brokerSignalFor(ProcessSignal signal) noexcept
{
    switch (signal)
    {
    case ProcessSignal::Hangup:
        return Broker::Signal::Hangup;
    case ProcessSignal::Interrupt:
        return Broker::Signal::Interrupt;
    case ProcessSignal::Quit:
        return Broker::Signal::Quit;
    case ProcessSignal::User1:
        return Broker::Signal::User1;
    case ProcessSignal::User2:
        return Broker::Signal::User2;
    }
    return Broker::Signal::Hangup; // unreachable: every signal is above
}

std::optional<Broker::Request> brokerRequestFor(const AuthorizedAction& action)
{
    using Kind = AuthorizedAction::Kind;
    switch (action.kind)
    {
    case Kind::Terminate:
        return Broker::SignalRequest{.target = action.target, .signal = Broker::Signal::Terminate};
    case Kind::Kill:
        return Broker::SignalRequest{.target = action.target, .signal = Broker::Signal::Kill};
    case Kind::Stop:
        return Broker::SignalRequest{.target = action.target, .signal = Broker::Signal::Suspend};
    case Kind::Resume:
        return Broker::SignalRequest{.target = action.target, .signal = Broker::Signal::Resume};
    case Kind::SetPriority:
        return Broker::PriorityRequest{.target = action.target, .nice = action.nice};
    case Kind::SetIoPriority:
        return Broker::IoPriorityRequest{.target = action.target, .ioClass = action.ioClass, .level = action.level};
    case Kind::SetAffinity:
        return Broker::AffinityRequest{.target = action.target, .cpus = action.cpus};
    case Kind::SetSchedPolicy:
        return std::nullopt; // Linux's, and Linux has no helper since #3580: protocol v23 has no SetSchedPolicy
    case Kind::SendSignal:
        return Broker::SignalRequest{.target = action.target, .signal = brokerSignalFor(action.signal)};
    case Kind::SetEfficiencyMode:
        return Broker::EfficiencyModeRequest{.target = action.target, .on = action.efficiencyMode};
    case Kind::CloseConnection:
        return Broker::CloseConnectionRequest{
            .target = action.target,
            .socketInode = action.socketInode,
            .tcpConnection = action.tcpConnection,
        };
    case Kind::ThreadAction:
        return Broker::ThreadActionRequest{
            .target = action.target,
            .tid = action.tid,
            .threadCreateTime = action.threadCreateTime,
            .action = action.threadAction,
        };
    }
    return Broker::SignalRequest{.target = action.target, .signal = Broker::Signal::Terminate}; // unreachable: every kind is above
}

std::unique_ptr<IProcessActions> withPrivilegedBroker(std::unique_ptr<IProcessActions> actions, std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || actions == nullptr)
    {
        return actions;
    }
    return std::make_unique<BrokeredProcessActions>(std::move(actions), std::move(broker));
}

BrokeredProcessActions::BrokeredProcessActions(std::unique_ptr<IProcessActions> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

ProcessActionCapabilities BrokeredProcessActions::actionCapabilities() const
{
    ProcessActionCapabilities capabilities = m_Inner->actionCapabilities();
    capabilities.canAuthorizeInteractively = (m_Broker != nullptr);
    // No pipe call here: Process Details asks every frame, on the UI thread. Windows' I/O hints have no Realtime
    // class: High is offered anyway, and a refusal is retried (#3362). The Realtime I/O class and real-time
    // scheduling the Linux helper could set went with it (#3580).
    return capabilities;
}

ProcessActionResult BrokeredProcessActions::retried(ProcessActionResult result, const AuthorizedAction& action)
{
    if (!result.permissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    const std::optional<Broker::Request> request = requestFor(action);
    if (!request.has_value())
    {
        return result; // not an action the helper does (#3580): the refusal stands
    }
    const std::expected<Broker::Response, std::string> response = m_Broker->call(*request, std::stop_token{});
    if (!response.has_value())
    {
        // The helper exited, timed out or broke between connected() and the call (#2414). The refusal
        // stands, so the UI still offers Authenticate and retry, which starts a fresh helper.
        result.errorMessage += " (the administrator helper couldn't be asked: " + response.error() + ")";
        return result;
    }
    return finished(actionResultOf(response), action, *request);
}

std::optional<Broker::Request> BrokeredProcessActions::requestFor(const AuthorizedAction& action) const
{
    if (action.kind == AuthorizedAction::Kind::SetEfficiencyMode && !action.efficiencyMode)
    {
        if (const std::optional<std::int32_t> nice = m_Inner->efficiencyNiceToRestore(action.target); nice.has_value())
        {
            return Broker::PriorityRequest{.target = action.target, .nice = *nice};
        }
        // Windows (#3327): the class TaskSmack remembers the on replaced, which the helper may never have seen.
        return Broker::EfficiencyModeRequest{
            .target = action.target,
            .on = false,
            .restorePriorityClass = m_Inner->efficiencyPriorityClassToRestore(action.target).value_or(0),
        };
    }
    return brokerRequestFor(action);
}

ProcessActionResult
BrokeredProcessActions::finished(ProcessActionResult result, const AuthorizedAction& action, const Broker::Request& request)
{
    if (!result.success || action.kind != AuthorizedAction::Kind::SetEfficiencyMode)
    {
        return result;
    }
    // Windows (#3327): TaskSmack, not the helper, keeps the class an on replaced, so a later off restores it
    // wherever it runs; an off the helper finished has spent it.
    if (std::holds_alternative<Broker::EfficiencyModeRequest>(request))
    {
        if (!action.efficiencyMode)
        {
            m_Inner->forgetEfficiencyPriorityClass(action.target);
        }
        else if (result.priorityClassBefore != 0)
        {
            m_Inner->rememberEfficiencyPriorityClass(action.target, result.priorityClassBefore);
        }
        return result;
    }
    if (!std::holds_alternative<Broker::PriorityRequest>(request))
    {
        return result;
    }
    const ProcessActionResult rest = m_Inner->setEfficiencyMode(action.target, false);
    const std::string note =
        rest.success ? rest.successNote : "its priority is back, but its cgroup's CPU weight wasn't put back: " + rest.errorMessage;
    if (!note.empty())
    {
        result.successNote += (result.successNote.empty() ? "" : "; ") + note;
    }
    return result;
}

ProcessActionResult BrokeredProcessActions::terminate(const ProcessTarget& target)
{
    return retried(m_Inner->terminate(target), {.kind = AuthorizedAction::Kind::Terminate, .target = target});
}

ProcessActionResult BrokeredProcessActions::kill(const ProcessTarget& target)
{
    return retried(m_Inner->kill(target), {.kind = AuthorizedAction::Kind::Kill, .target = target});
}

ProcessActionResult BrokeredProcessActions::stop(const ProcessTarget& target)
{
    return retried(m_Inner->stop(target), {.kind = AuthorizedAction::Kind::Stop, .target = target});
}

ProcessActionResult BrokeredProcessActions::resume(const ProcessTarget& target)
{
    return retried(m_Inner->resume(target), {.kind = AuthorizedAction::Kind::Resume, .target = target});
}

ProcessActionResult BrokeredProcessActions::sendSignal(const ProcessTarget& target, ProcessSignal signal)
{
    return retried(m_Inner->sendSignal(target, signal), {.kind = AuthorizedAction::Kind::SendSignal, .target = target, .signal = signal});
}

ProcessActionResult BrokeredProcessActions::setPriority(const ProcessTarget& target, std::int32_t nice)
{
    return retried(m_Inner->setPriority(target, nice), {.kind = AuthorizedAction::Kind::SetPriority, .target = target, .nice = nice});
}

ProcessActionResult BrokeredProcessActions::setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, std::int32_t level)
{
    return retried(m_Inner->setIoPriority(target, ioClass, level),
                   {.kind = AuthorizedAction::Kind::SetIoPriority, .target = target, .ioClass = ioClass, .level = level});
}

ProcessActionResult BrokeredProcessActions::setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
{
    return retried(m_Inner->setAffinity(target, cpus), {.kind = AuthorizedAction::Kind::SetAffinity, .target = target, .cpus = cpus});
}

ProcessActionResult BrokeredProcessActions::setSchedPolicy(const ProcessTarget& target,
                                                           SchedPolicy policy,
                                                           std::int32_t rtPriority,
                                                           RealtimeThreads realtimeThreads)
{
    return m_Inner->setSchedPolicy(target, policy, rtPriority, realtimeThreads); // Linux's: no helper retries it (#3580)
}

ProcessActionResult BrokeredProcessActions::closeConnection(const ProcessTarget& target, std::uint64_t socketInode)
{
    return retried(m_Inner->closeConnection(target, socketInode),
                   {.kind = AuthorizedAction::Kind::CloseConnection, .target = target, .socketInode = socketInode});
}

ProcessActionResult BrokeredProcessActions::closeTcpConnection(const ProcessTarget& target, const TcpConnectionToClose& connection)
{
    return retried(m_Inner->closeTcpConnection(target, connection),
                   {.kind = AuthorizedAction::Kind::CloseConnection, .target = target, .tcpConnection = connection});
}

ProcessActionResult
BrokeredProcessActions::actOnThread(const ProcessTarget& target, std::int32_t tid, std::uint64_t threadCreateTime, ThreadAction action)
{
    return retried(m_Inner->actOnThread(target, tid, threadCreateTime, action),
                   {
                       .kind = AuthorizedAction::Kind::ThreadAction,
                       .target = target,
                       .tid = tid,
                       .threadCreateTime = threadCreateTime,
                       .threadAction = action,
                   });
}

ProcessActionResult BrokeredProcessActions::setEfficiencyMode(const ProcessTarget& target, bool on)
{
    return retried(m_Inner->setEfficiencyMode(target, on),
                   {.kind = AuthorizedAction::Kind::SetEfficiencyMode, .target = target, .efficiencyMode = on});
}

ProcessActionResult BrokeredProcessActions::runAuthorized(const AuthorizedAction& action, const std::stop_token& stopToken)
{
    if (m_Broker == nullptr)
    {
        return ProcessActionResult::error("Acting as an administrator isn't available");
    }
    if (!brokerRequestFor(action).has_value()) // before any prompt: the helper couldn't do it
    {
        return ProcessActionResult::error(std::string(NOT_THROUGH_THE_HELPER));
    }
    // An Efficiency mode off with no nice left to put back (someone changed it since the refusal) may need no
    // administrator now, and only the in-process off puts back the weight and forgets the on (#3296).
    if (action.kind == AuthorizedAction::Kind::SetEfficiencyMode && !action.efficiencyMode &&
        !m_Inner->efficiencyNiceToRestore(action.target).has_value())
    {
        if (ProcessActionResult inProcess = m_Inner->setEfficiencyMode(action.target, false); !inProcess.permissionDenied)
        {
            return inProcess;
        }
    }
    if (!m_Broker->connected())
    {
        if (const std::expected<void, std::string> connected = m_Broker->connect(stopToken); !connected.has_value())
        {
            return ProcessActionResult::error("Couldn't act as an administrator: " + connected.error());
        }
    }
    // requestFor() asks efficiencyNiceToRestore() again: authorizing takes as long as the user does, and a Windows
    // Realtime retry whose process left the High since is sent as the ordinary off, never as Realtime (#3606).
    const std::optional<Broker::Request> request = requestFor(action);
    if (!request.has_value())
    {
        return ProcessActionResult::error(std::string(NOT_THROUGH_THE_HELPER));
    }
    return finished(actionResultOf(m_Broker->call(*request, stopToken)), action, *request);
}

IoPriorityReadResult BrokeredProcessActions::getIoPriority(const ProcessTarget& target)
{
    return m_Inner->getIoPriority(target);
}

ProcessActionResult BrokeredProcessActions::launchSyscallTrace(const ProcessTarget& target)
{
    return m_Inner->launchSyscallTrace(target);
}

ProcessActionResult BrokeredProcessActions::openFileLocation(const ProcessTarget& target)
{
    return m_Inner->openFileLocation(target); // the user's file manager, as the user: never the helper's
}

std::expected<std::string, std::string> BrokeredProcessActions::imageSha256(const ProcessTarget& target, const std::stop_token& stop)
{
    return m_Inner->imageSha256(target, stop); // a file read for Look up on VirusTotal (#3094): never the helper's
}

ProcessActionResult BrokeredProcessActions::runProgram(const RunProgramRequest& request)
{
    return m_Inner->runProgram(request); // its own pkexec or UAC prompt when elevated: the helper never starts programs
}

ProcessDumpResult
BrokeredProcessActions::createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    return m_Inner->createDump(target, directory, stopToken);
}

DumpSpace BrokeredProcessActions::dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
{
    return m_Inner->dumpSpace(target, directory);
}

FolderChoice BrokeredProcessActions::chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow)
{
    return m_Inner->chooseDumpFolder(initial, ownerWindow);
}

WaitChainReadResult BrokeredProcessActions::readWaitChains(const ProcessTarget& target, const std::stop_token& stopToken)
{
    return m_Inner->readWaitChains(target, stopToken);
}

CpuAffinityLayout BrokeredProcessActions::affinityLayout() const
{
    return m_Inner->affinityLayout();
}

std::unique_ptr<IProcessEnvironmentReader> withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader> reader,
                                                                std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasEnvironment())
    {
        return reader;
    }
    return std::make_unique<BrokeredEnvironmentReader>(std::move(reader), std::move(broker));
}

BrokeredEnvironmentReader::BrokeredEnvironmentReader(std::unique_ptr<IProcessEnvironmentReader> inner,
                                                     std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredEnvironmentReader::hasEnvironment() const
{
    return m_Inner->hasEnvironment();
}

EnvironmentReadResult BrokeredEnvironmentReader::readEnvironment(const ProcessTarget& target)
{
    EnvironmentReadResult result = m_Inner->readEnvironment(target);
    if (result.status != EnvironmentReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredEnvironmentReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

EnvironmentReadResult BrokeredEnvironmentReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readEnvironment(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        // The prompt was dismissed or the helper couldn't start: the refusal stands.
        return {.status = EnvironmentReadStatus::PermissionDenied, .variables = {}};
    }
    return viaBroker(target, stopToken);
}

EnvironmentReadResult BrokeredEnvironmentReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::EnvironmentRequest{.target = target}, stopToken);
    if (response.has_value())
    {
        if (const auto* reply = std::get_if<Broker::EnvironmentReply>(&*response))
        {
            return reply->result;
        }
    }
    return {.status = EnvironmentReadStatus::Failed, .variables = {}};
}

std::unique_ptr<IProcessModulesReader> withPrivilegedBroker(std::unique_ptr<IProcessModulesReader> reader,
                                                            std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasModules())
    {
        return reader;
    }
    return std::make_unique<BrokeredModulesReader>(std::move(reader), std::move(broker));
}

std::unique_ptr<IProcessOpenFilesReader> withPrivilegedBroker(std::unique_ptr<IProcessOpenFilesReader> reader,
                                                              std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasOpenFiles())
    {
        return reader;
    }
    return std::make_unique<BrokeredOpenFilesReader>(std::move(reader), std::move(broker));
}

BrokeredModulesReader::BrokeredModulesReader(std::unique_ptr<IProcessModulesReader> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredModulesReader::hasModules() const
{
    return m_Inner->hasModules();
}

ModulesReadResult BrokeredModulesReader::readModules(const ProcessTarget& target)
{
    ModulesReadResult result = m_Inner->readModules(target);
    if (result.status != ModulesReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredModulesReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

ModulesReadResult BrokeredModulesReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readModules(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        return {.status = ModulesReadStatus::PermissionDenied, .modules = {}, .detail = {}}; // the refusal stands
    }
    return viaBroker(target, stopToken);
}

ModulesReadResult BrokeredModulesReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::ModulesRequest{.target = target}, stopToken);
    if (!response.has_value())
    {
        return {
            .status = ModulesReadStatus::Failed,
            .modules = {},
            .detail = "The administrator helper couldn't be asked: " + response.error(),
        };
    }
    if (const auto* reply = std::get_if<Broker::ModulesReply>(&*response))
    {
        return reply->result;
    }
    return {.status = ModulesReadStatus::Failed, .modules = {}, .detail = "The administrator helper gave an unexpected answer"};
}

BrokeredOpenFilesReader::BrokeredOpenFilesReader(std::unique_ptr<IProcessOpenFilesReader> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredOpenFilesReader::hasOpenFiles() const
{
    return m_Inner->hasOpenFiles();
}

std::size_t BrokeredOpenFilesReader::handlesScanned() const noexcept
{
    return m_Inner->handlesScanned();
}

void BrokeredOpenFilesReader::beginBatch()
{
    m_Inner->beginBatch();
    m_InBatch = true;
    m_BatchUnanswered = false;
    m_Expected.clear();
    m_NextExpected = 0;
}

void BrokeredOpenFilesReader::endBatch() noexcept
{
    m_InBatch = false;
    m_BatchUnanswered = false;
    m_Expected.clear();
    m_NextExpected = 0;
    m_Inner->endBatch();
}

void BrokeredOpenFilesReader::expectAuthorizedReads(std::span<const ProcessTarget> targets)
{
    if (!m_InBatch)
    {
        return; // outside a batch every read is fresh
    }
    m_Expected.clear();
    m_Expected.reserve(targets.size());
    for (const ProcessTarget& target : targets)
    {
        m_Expected.push_back(Expected{.target = target, .answered = false, .result = std::nullopt});
    }
    m_NextExpected = 0;
}

std::optional<OpenFilesReadResult> BrokeredOpenFilesReader::fromBatch(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!m_InBatch || m_BatchUnanswered || m_Expected.empty())
    {
        return std::nullopt;
    }
    // Reads come in the order expected, so the lookup starts where the last one left off, then wraps.
    std::optional<std::size_t> found;
    for (std::size_t step = 0; step < m_Expected.size() && !found.has_value(); ++step)
    {
        const std::size_t index = (m_NextExpected + step) % m_Expected.size();
        if (m_Expected[index].target == target)
        {
            found = index;
        }
    }
    if (!found.has_value())
    {
        return std::nullopt;
    }
    m_NextExpected = *found + 1;
    Expected& expected = m_Expected[*found];
    if (!expected.answered)
    {
        askBatch(*found, stopToken);
    }
    if (!expected.result.has_value())
    {
        return std::nullopt; // not answered, or already taken: read alone
    }
    std::optional<OpenFilesReadResult> result = std::move(expected.result);
    expected.result.reset();
    return result;
}

void BrokeredOpenFilesReader::askBatch(std::size_t from, const std::stop_token& stopToken)
{
    Broker::OpenFilesBatchRequest request;
    std::vector<std::size_t> asked; // the m_Expected index of each target in the request
    for (std::size_t index = from; index < m_Expected.size() && request.targets.size() < Broker::MAX_OPEN_FILES_BATCH; ++index)
    {
        if (!m_Expected[index].answered)
        {
            request.targets.push_back(m_Expected[index].target);
            asked.push_back(index);
        }
    }
    std::expected<Broker::Response, std::string> response = m_Broker->call(request, stopToken);
    if (!response.has_value())
    {
        // The channel broke or timed out: the first target says why, and the rest are asked for one at a time, as
        // before (the broker's next call then says it isn't connected).
        m_BatchUnanswered = true;
        OpenFilesReadResult failed;
        failed.status = OpenFilesReadStatus::Failed;
        failed.detail = "The administrator helper couldn't be asked: " + response.error();
        m_Expected[from].answered = true;
        m_Expected[from].result = std::move(failed);
        return;
    }
    auto* const reply = std::get_if<Broker::OpenFilesBatchReply>(&*response);
    if (reply == nullptr || reply->results.empty() || reply->results.size() > asked.size())
    {
        m_BatchUnanswered = true; // a helper that doesn't read batches: one process at a time from here
        return;
    }
    for (std::size_t i = 0; i < reply->results.size(); ++i)
    {
        Expected& expected = m_Expected[asked[i]];
        expected.answered = true;
        expected.result = std::move(reply->results[i]);
    }
}

OpenFilesReadResult BrokeredOpenFilesReader::readOpenFiles(const ProcessTarget& target)
{
    OpenFilesReadResult result = m_Inner->readOpenFiles(target);
    if (result.status != OpenFilesReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredOpenFilesReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

OpenFilesReadResult BrokeredOpenFilesReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readOpenFiles(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        OpenFilesReadResult refused; // the refusal stands
        refused.status = OpenFilesReadStatus::PermissionDenied;
        return refused;
    }
    if (std::optional<OpenFilesReadResult> batched = fromBatch(target, stopToken); batched.has_value())
    {
        return std::move(*batched);
    }
    return viaBroker(target, stopToken);
}

OpenFilesReadResult BrokeredOpenFilesReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::OpenFilesRequest{.target = target}, stopToken);
    OpenFilesReadResult failed;
    failed.status = OpenFilesReadStatus::Failed;
    if (!response.has_value())
    {
        failed.detail = "The administrator helper couldn't be asked: " + response.error();
        return failed;
    }
    if (const auto* reply = std::get_if<Broker::OpenFilesReply>(&*response))
    {
        return reply->result;
    }
    failed.detail = "The administrator helper gave an unexpected answer";
    return failed;
}

std::unique_ptr<IProcessConnectionsReader> withPrivilegedBroker(std::unique_ptr<IProcessConnectionsReader> reader,
                                                                std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasConnections())
    {
        return reader;
    }
    return std::make_unique<BrokeredConnectionsReader>(std::move(reader), std::move(broker));
}

BrokeredConnectionsReader::BrokeredConnectionsReader(std::unique_ptr<IProcessConnectionsReader> inner,
                                                     std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredConnectionsReader::hasConnections() const
{
    return m_Inner->hasConnections();
}

ConnectionsReadResult BrokeredConnectionsReader::readConnections(const ProcessTarget& target)
{
    ConnectionsReadResult result = m_Inner->readConnections(target);
    if (result.status != ConnectionsReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredConnectionsReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

ConnectionsReadResult BrokeredConnectionsReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readConnections(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        return {.status = ConnectionsReadStatus::PermissionDenied, .connections = {}, .detail = {}}; // the refusal stands
    }
    return viaBroker(target, stopToken);
}

ConnectionsReadResult BrokeredConnectionsReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::ConnectionsRequest{.target = target}, stopToken);
    if (!response.has_value())
    {
        return {
            .status = ConnectionsReadStatus::Failed,
            .connections = {},
            .detail = "The administrator helper couldn't be asked: " + response.error(),
        };
    }
    if (const auto* reply = std::get_if<Broker::ConnectionsReply>(&*response))
    {
        return reply->result;
    }
    return {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = "The administrator helper gave an unexpected answer"};
}

std::unique_ptr<IProcessThreadsReader> withPrivilegedBroker(std::unique_ptr<IProcessThreadsReader> reader,
                                                            std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasThreads())
    {
        return reader;
    }
    return std::make_unique<BrokeredThreadsReader>(std::move(reader), std::move(broker));
}

BrokeredThreadsReader::BrokeredThreadsReader(std::unique_ptr<IProcessThreadsReader> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredThreadsReader::hasThreads() const
{
    return m_Inner->hasThreads();
}

ThreadsReadResult BrokeredThreadsReader::readThreads(const ProcessTarget& target)
{
    ThreadsReadResult result = m_Inner->readThreads(target);
    if (result.status != ThreadsReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredThreadsReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

ThreadsReadResult BrokeredThreadsReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readThreads(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        return {.status = ThreadsReadStatus::PermissionDenied, .threads = {}, .detail = {}}; // the refusal stands
    }
    return viaBroker(target, stopToken);
}

ThreadsReadResult BrokeredThreadsReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::ThreadsRequest{.target = target}, stopToken);
    if (!response.has_value())
    {
        return {
            .status = ThreadsReadStatus::Failed,
            .threads = {},
            .detail = "The administrator helper couldn't be asked: " + response.error(),
        };
    }
    if (const auto* reply = std::get_if<Broker::ThreadsReply>(&*response))
    {
        return reply->result;
    }
    return {.status = ThreadsReadStatus::Failed, .threads = {}, .detail = "The administrator helper gave an unexpected answer"};
}

std::unique_ptr<IProcessMemoryMapReader> withPrivilegedBroker(std::unique_ptr<IProcessMemoryMapReader> reader,
                                                              std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || reader == nullptr || !reader->hasMemoryMap())
    {
        return reader;
    }
    return std::make_unique<BrokeredMemoryMapReader>(std::move(reader), std::move(broker));
}

BrokeredMemoryMapReader::BrokeredMemoryMapReader(std::unique_ptr<IProcessMemoryMapReader> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

bool BrokeredMemoryMapReader::hasMemoryMap() const
{
    return m_Inner->hasMemoryMap();
}

MemoryMapReadResult BrokeredMemoryMapReader::readMemoryMap(const ProcessTarget& target)
{
    MemoryMapReadResult result = m_Inner->readMemoryMap(target);
    if (result.status != MemoryMapReadStatus::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    return viaBroker(target, std::stop_token{});
}

bool BrokeredMemoryMapReader::canReadAuthorized() const
{
    return m_Broker != nullptr && m_Broker->readsOtherUsers();
}

MemoryMapReadResult BrokeredMemoryMapReader::readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken)
{
    if (!canReadAuthorized()) // no helper, or none that could read it (#2550)
    {
        return m_Inner->readMemoryMap(target);
    }
    if (!m_Broker->connected() && !m_Broker->connect(stopToken).has_value())
    {
        return {.status = MemoryMapReadStatus::PermissionDenied, .mappings = {}, .detail = {}, .truncated = false}; // the refusal stands
    }
    return viaBroker(target, stopToken);
}

MemoryMapReadResult BrokeredMemoryMapReader::viaBroker(const ProcessTarget& target, const std::stop_token& stopToken)
{
    const std::expected<Broker::Response, std::string> response = m_Broker->call(Broker::MemoryMapRequest{.target = target}, stopToken);
    if (!response.has_value())
    {
        return {
            .status = MemoryMapReadStatus::Failed,
            .mappings = {},
            .detail = "The administrator helper couldn't be asked: " + response.error(),
            .truncated = false,
        };
    }
    if (const auto* reply = std::get_if<Broker::MemoryMapReply>(&*response))
    {
        return reply->result;
    }
    return {
        .status = MemoryMapReadStatus::Failed,
        .mappings = {},
        .detail = "The administrator helper gave an unexpected answer",
        .truncated = false,
    };
}

} // namespace Platform
