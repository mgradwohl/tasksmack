#include "BrokeredProcessActions.h"

#include "Domain/SamplingConfig.h"
#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessProbe.h"
#include "Platform/IUdpTrafficCollector.h"
#include "Platform/ProcessTypes.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace Platform
{

Broker::Request brokerRequestFor(const AuthorizedAction& action)
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
    if (m_Broker != nullptr && m_Broker->connected() && capabilities.canSetIoPriority)
    {
        capabilities.canSetRealtimeIoPriority = true; // the helper, as root, has CAP_SYS_NICE
    }
    return capabilities;
}

ProcessActionResult BrokeredProcessActions::retried(ProcessActionResult result, const AuthorizedAction& action)
{
    if (!result.permissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return result;
    }
    const std::expected<Broker::Response, std::string> response = m_Broker->call(brokerRequestFor(action), std::stop_token{});
    if (!response.has_value())
    {
        // The helper exited, timed out or broke between connected() and the call (#2414). The refusal
        // stands, so the UI still offers Authenticate and retry, which starts a fresh helper.
        result.errorMessage += " (the administrator helper couldn't be asked: " + response.error() + ")";
        return result;
    }
    return actionResultOf(response);
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

ProcessActionResult BrokeredProcessActions::runAuthorized(const AuthorizedAction& action, const std::stop_token& stopToken)
{
    if (m_Broker == nullptr)
    {
        return ProcessActionResult::error("Acting as an administrator isn't available");
    }
    if (!m_Broker->connected())
    {
        if (const std::expected<void, std::string> connected = m_Broker->connect(stopToken); !connected.has_value())
        {
            return ProcessActionResult::error("Couldn't act as an administrator: " + connected.error());
        }
    }
    return actionResultOf(m_Broker->call(brokerRequestFor(action), stopToken));
}

IoPriorityReadResult BrokeredProcessActions::getIoPriority(const ProcessTarget& target)
{
    return m_Inner->getIoPriority(target);
}

ProcessActionResult BrokeredProcessActions::launchSyscallTrace(const ProcessTarget& target)
{
    return m_Inner->launchSyscallTrace(target);
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
        return {.status = ModulesReadStatus::Failed,
                .modules = {},
                .detail = "The administrator helper couldn't be asked: " + response.error()};
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
        return {.status = ConnectionsReadStatus::Failed,
                .connections = {},
                .detail = "The administrator helper couldn't be asked: " + response.error()};
    }
    if (const auto* reply = std::get_if<Broker::ConnectionsReply>(&*response))
    {
        return reply->result;
    }
    return {.status = ConnectionsReadStatus::Failed, .connections = {}, .detail = "The administrator helper gave an unexpected answer"};
}

std::unique_ptr<IProcessProbe> withPrivilegedBroker(std::unique_ptr<IProcessProbe> probe, std::shared_ptr<IPrivilegedBroker> broker)
{
    if (broker == nullptr || probe == nullptr || !probe->capabilities().hasMemoryRollup)
    {
        return probe;
    }
    return std::make_unique<BrokeredProcessProbe>(std::move(probe), std::move(broker));
}

BrokeredProcessProbe::BrokeredProcessProbe(std::unique_ptr<IProcessProbe> inner, std::shared_ptr<IPrivilegedBroker> broker)
    : m_Inner(std::move(inner)), m_Broker(std::move(broker))
{}

std::vector<ProcessCounters> BrokeredProcessProbe::enumerate()
{
    std::vector<ProcessCounters> counters = m_Inner->enumerate();
    readRefusedCounters(counters);
    return counters;
}

void BrokeredProcessProbe::readRefusedCounters(std::vector<ProcessCounters>& counters)
{
    if (m_CountersRefused || m_Broker == nullptr || !m_Broker->connected())
    {
        return;
    }
    // Sockets at the inner inode-to-PID map's cadence, not every refresh: reading every link is the costly part.
    const auto now = std::chrono::steady_clock::now();
    Broker::ProcessCountersRequest request;
    request.withSockets = now - m_SocketsReadAt >= std::chrono::milliseconds{Domain::Sampling::INODE_PID_CACHE_TTL_MS} &&
                          m_Inner->capabilities().hasNetworkCounters;
    for (const ProcessCounters& process : counters)
    {
        const bool refused =
            !process.handleCountAvailable || !process.ioCountersAvailable || (request.withSockets && !process.networkCountersAvailable);
        if (refused && request.targets.size() < Broker::MAX_COUNTER_TARGETS)
        {
            request.targets.push_back({.pid = process.pid, .startTimeTicks = process.startTimeTicks});
        }
    }
    // Between socket reads, the last one's processes keep their connections' attribution.
    const auto keepAttribution = [this, &counters]
    {
        const std::scoped_lock lock(m_SocketOwnersMutex);
        for (ProcessCounters& process : counters)
        {
            if (const auto read = m_SocketsReadFor.find(process.pid);
                read != m_SocketsReadFor.end() && read->second == process.startTimeTicks)
            {
                process.networkCountersAvailable = true;
            }
        }
    };
    if (request.targets.empty())
    {
        keepAttribution();
        return;
    }
    // As the PSS read: on the sampler's thread, never behind another call, and briefly; a skipped read
    // shows the refusals until the next refresh.
    constexpr std::chrono::milliseconds COUNTERS_TIMEOUT{1000};
    const std::expected<Broker::Response, std::string> response = m_Broker->callIfIdle(request, COUNTERS_TIMEOUT);
    if (!response.has_value())
    {
        keepAttribution();
        return;
    }
    if (std::holds_alternative<Broker::ErrorReply>(*response))
    {
        m_CountersRefused = true;
        return;
    }
    const auto* reply = std::get_if<Broker::ProcessCountersReply>(&*response);
    if (reply == nullptr)
    {
        keepAttribution();
        return;
    }
    if (request.withSockets)
    {
        m_SocketsReadAt = now;
        std::unordered_map<std::uint64_t, SocketOwner> owners;
        std::unordered_map<std::int32_t, std::uint64_t> readFor;
        for (const Broker::ProcessCountersEntry& entry : reply->entries)
        {
            if (!entry.socketsRead)
            {
                continue;
            }
            readFor.insert_or_assign(entry.target.pid, entry.target.startTimeTicks);
            for (const std::uint64_t inode : entry.socketInodes)
            {
                owners.insert_or_assign(inode, SocketOwner{.pid = entry.target.pid, .startTimeTicks = entry.target.startTimeTicks});
            }
        }
        const std::scoped_lock lock(m_SocketOwnersMutex);
        m_SocketOwners = std::move(owners);
        m_SocketsReadFor = std::move(readFor);
    }
    keepAttribution();
    // Entries come in the request's order, which is the counters', with gone processes left out.
    auto process = counters.begin();
    for (const Broker::ProcessCountersEntry& entry : reply->entries)
    {
        process = std::find_if(process,
                               counters.end(),
                               [&entry](const ProcessCounters& candidate)
                               { return candidate.pid == entry.target.pid && candidate.startTimeTicks == entry.target.startTimeTicks; });
        if (process == counters.end())
        {
            break;
        }
        if (!process->handleCountAvailable && entry.handleCountRead)
        {
            process->handleCount = entry.handleCount;
            process->handleCountAvailable = true;
        }
        if (!process->ioCountersAvailable && entry.ioRead)
        {
            process->readBytes = entry.readBytes;
            process->writeBytes = entry.writeBytes;
            process->ioCountersAvailable = true;
        }
    }
}

ProcessCapabilities BrokeredProcessProbe::capabilities() const
{
    return m_Inner->capabilities();
}

std::uint64_t BrokeredProcessProbe::totalCpuTime() const
{
    return m_Inner->totalCpuTime();
}

long BrokeredProcessProbe::ticksPerSecond() const
{
    return m_Inner->ticksPerSecond();
}

std::uint64_t BrokeredProcessProbe::systemTotalMemory() const
{
    return m_Inner->systemTotalMemory();
}

std::optional<PackageEnergyReading> BrokeredProcessProbe::readPackageEnergy() const
{
    return m_Inner->readPackageEnergy();
}

SocketTrafficReading BrokeredProcessProbe::readSocketTraffic() const
{
    SocketTrafficReading reading = m_Inner->readSocketTraffic();
    // The connections the inner map left unowned, held by processes whose fd links only the helper reads.
    const std::scoped_lock lock(m_SocketOwnersMutex);
    if (m_SocketOwners.empty())
    {
        return reading;
    }
    for (SocketTrafficSample& sample : reading.sockets)
    {
        if (sample.pid != 0)
        {
            continue;
        }
        if (const auto owner = m_SocketOwners.find(sample.key); owner != m_SocketOwners.end())
        {
            sample.pid = owner->second.pid;
            sample.ownerStartTimeTicks = owner->second.startTimeTicks;
        }
    }
    return reading;
}

MemoryRollupRead BrokeredProcessProbe::readMemoryRollup(std::int32_t pid, std::uint64_t startTimeTicks) const
{
    MemoryRollupRead read = m_Inner->readMemoryRollup(pid, startTimeTicks);
    if (read.has_value() || read.error() != MemoryRollupFailure::PermissionDenied || m_Broker == nullptr || !m_Broker->connected())
    {
        return read;
    }
    // On the sampler's thread: never behind another call, and briefly, so the Processes table never stalls
    // on the helper (#2369). A skipped read just shows the refusal until the next refresh.
    constexpr std::chrono::milliseconds ROLLUP_TIMEOUT{1000};
    const std::expected<Broker::Response, std::string> response =
        m_Broker->callIfIdle(Broker::MemoryRollupRequest{.target = {.pid = pid, .startTimeTicks = startTimeTicks}}, ROLLUP_TIMEOUT);
    if (response.has_value())
    {
        if (const auto* reply = std::get_if<Broker::MemoryRollupReply>(&*response))
        {
            return reply->result;
        }
    }
    return read; // the helper couldn't be asked: the refusal stands
}

void BrokeredProcessProbe::setSocketStatsCacheTtl(std::chrono::milliseconds ttlMs)
{
    m_Inner->setSocketStatsCacheTtl(ttlMs);
}

void BrokeredProcessProbe::setUdpTrafficCollector(const std::shared_ptr<IUdpTrafficCollector>& collector)
{
    m_Inner->setUdpTrafficCollector(collector);
}

} // namespace Platform
