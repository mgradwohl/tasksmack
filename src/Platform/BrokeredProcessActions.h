#pragma once

// Process actions that an administrator can finish (#2308; #2181): an IProcessActions decorator over the
// in-process implementation and the privileged broker (IPrivilegedBroker: Windows' elevated helper, #2311;
// Linux has none since #3580). Each action is tried in-process first, as before. Only a failure the OS gave for want of
// permission (ProcessActionResult::permissionDenied) goes further:
//
// - while the helper is running (the user authenticated a few minutes ago), the action is retried through
//   it at once, with no prompt; if the helper can't be asked after all (it exited or timed out), the
//   refusal is returned as below (#2414);
// - otherwise the refusal is returned as it is, and the UI offers "Authenticate and retry"
//   (actionCapabilities().canAuthorizeInteractively), which calls runAuthorized(): that asks for the
//   password, starts the helper, and sends the action through it.
//
// The helper re-checks every target by PID and start time with its capabilities, so a retried action never reaches a
// process other than the one the in-process attempt was for. A failure from the helper (running with its capabilities)
// is never permissionDenied, so nothing is retried twice. App composes one per IProcessActions, all over
// the one broker, and only where the helper is installed and TaskSmack isn't already an administrator.

#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessConnections.h"
#include "Platform/IProcessEnvironment.h"
#include "Platform/IProcessModules.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IProcessThreads.h"
#include "Platform/IoPriorityTypes.h"
#include "Platform/SchedPolicy.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace Platform
{

/// @p actions decorated with @p broker (BrokeredProcessActions), or @p actions as they are when there is
/// no broker.
[[nodiscard]] std::unique_ptr<IProcessActions> withPrivilegedBroker(std::unique_ptr<IProcessActions> actions,
                                                                    std::shared_ptr<IPrivilegedBroker> broker);

/// The broker request that does @p action; nullopt for one no request does: SetSchedPolicy, Linux's, whose
/// request protocol v23 dropped with the Linux helper (#3580).
[[nodiscard]] std::optional<Broker::Request> brokerRequestFor(const AuthorizedAction& action);

/// The Signal request's value for @p signal (#3063).
[[nodiscard]] Broker::Signal brokerSignalFor(ProcessSignal signal) noexcept;

class BrokeredProcessActions final : public IProcessActions
{
  public:
    BrokeredProcessActions(std::unique_ptr<IProcessActions> inner, std::shared_ptr<IPrivilegedBroker> broker);

    /// The in-process capabilities, plus canAuthorizeInteractively. Cheap for the UI thread every frame: no
    /// pipe call.
    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;

    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult sendSignal(const ProcessTarget& target, ProcessSignal signal) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, std::int32_t nice) override;
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, std::int32_t level) override;
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;
    /// The in-process actions' own (#3580): no helper request sets a scheduling policy.
    [[nodiscard]] ProcessActionResult
    setSchedPolicy(const ProcessTarget& target, SchedPolicy policy, std::int32_t rtPriority, RealtimeThreads realtimeThreads) override;
    [[nodiscard]] ProcessActionResult setEfficiencyMode(const ProcessTarget& target, bool on) override;
    [[nodiscard]] ProcessActionResult closeConnection(const ProcessTarget& target, std::uint64_t socketInode) override;
    [[nodiscard]] ProcessActionResult closeTcpConnection(const ProcessTarget& target, const TcpConnectionToClose& connection) override;
    [[nodiscard]] ProcessActionResult
    actOnThread(const ProcessTarget& target, std::int32_t tid, std::uint64_t threadCreateTime, ThreadAction action) override;

    /// Asks for the password if the helper isn't running, then does @p action through it; an error, with no
    /// prompt, for one no request does (brokerRequestFor()).
    [[nodiscard]] ProcessActionResult runAuthorized(const AuthorizedAction& action, const std::stop_token& stopToken) override;

    // Not actions an administrator finishes: the in-process implementation's own.
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult launchSyscallTrace(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult openFileLocation(const ProcessTarget& target) override;
    [[nodiscard]] std::expected<std::string, std::string> imageSha256(const ProcessTarget& target, const std::stop_token& stop) override;
    [[nodiscard]] ProcessActionResult runProgram(const RunProgramRequest& request) override;
    [[nodiscard]] ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override;
    [[nodiscard]] DumpSpace dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory) override;
    [[nodiscard]] FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow) override;
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;
    /// In-process only (#3084): the helper serves no wait chain read, so an elevated process's threads read as access denied.
    [[nodiscard]] WaitChainReadResult readWaitChains(const ProcessTarget& target, const std::stop_token& stopToken) override;

  private:
    /// @p result, or, when it is a permission refusal and the helper is running, @p action's result
    /// through the helper.
    [[nodiscard]] ProcessActionResult retried(ProcessActionResult result, const AuthorizedAction& action);
    /// @p action's request. An Efficiency mode off whose on ran in process is a priority change to the nice that on
    /// found (#3296): the helper never saw the on, and would otherwise put back nice 0. On Windows such an off carries
    /// the priority class TaskSmack remembers the on replaced (#3327), for the helper to restore rather than Normal.
    [[nodiscard]] std::optional<Broker::Request> requestFor(const AuthorizedAction& action) const;
    /// @p result of @p request, the helper's. After such a priority change, the in-process off runs again: the nice
    /// is no longer 19, so it puts back only the cgroup weight (the helper writes no files) and forgets the on. On
    /// Windows (#3327), an on the helper ran is remembered in process, by the class its reply says it replaced, and
    /// an off the helper ran forgets it.
    [[nodiscard]] ProcessActionResult finished(ProcessActionResult result, const AuthorizedAction& action, const Broker::Request& request);

    std::unique_ptr<IProcessActions> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' environment read that an administrator can finish (#2309), as
/// BrokeredProcessActions does for the actions: read in-process first; a PermissionDenied read goes
/// through the helper at once while it runs, and otherwise is returned for the UI to offer
/// "Authenticate to read" (readAuthorized()). The helper reads with its capabilities, with the same identity check.
class BrokeredEnvironmentReader final : public IProcessEnvironmentReader
{
  public:
    BrokeredEnvironmentReader(std::unique_ptr<IProcessEnvironmentReader> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasEnvironment() const override;
    [[nodiscard]] EnvironmentReadResult readEnvironment(const ProcessTarget& target) override;
    [[nodiscard]] bool canReadAuthorized() const override;
    [[nodiscard]] EnvironmentReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken) override;

  private:
    /// The read through the helper; a failure to ask it is a Failed read.
    [[nodiscard]] EnvironmentReadResult viaBroker(const ProcessTarget& target, const std::stop_token& stopToken);

    std::unique_ptr<IProcessEnvironmentReader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' modules list that an administrator can finish (#2309), as BrokeredEnvironmentReader.
class BrokeredModulesReader final : public IProcessModulesReader
{
  public:
    BrokeredModulesReader(std::unique_ptr<IProcessModulesReader> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasModules() const override;
    [[nodiscard]] ModulesReadResult readModules(const ProcessTarget& target) override;
    [[nodiscard]] bool canReadAuthorized() const override;
    [[nodiscard]] ModulesReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken) override;

  private:
    [[nodiscard]] ModulesReadResult viaBroker(const ProcessTarget& target, const std::stop_token& stopToken);

    std::unique_ptr<IProcessModulesReader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' open files list that an administrator can finish (#2309), as BrokeredEnvironmentReader.
class BrokeredOpenFilesReader final : public IProcessOpenFilesReader
{
  public:
    BrokeredOpenFilesReader(std::unique_ptr<IProcessOpenFilesReader> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasOpenFiles() const override;
    [[nodiscard]] OpenFilesReadResult readOpenFiles(const ProcessTarget& target) override;
    [[nodiscard]] std::size_t handlesScanned() const noexcept override;
    [[nodiscard]] bool canReadAuthorized() const override;
    [[nodiscard]] OpenFilesReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken) override;
    /// The inner reader's batch (#3357): its own reads come from one walk; the helper's each read for themselves.
    void beginBatch() override;
    void endBatch() noexcept override;

  private:
    [[nodiscard]] OpenFilesReadResult viaBroker(const ProcessTarget& target, const std::stop_token& stopToken);

    std::unique_ptr<IProcessOpenFilesReader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' connections list that an administrator can finish (#2309), as BrokeredEnvironmentReader.
class BrokeredConnectionsReader final : public IProcessConnectionsReader
{
  public:
    BrokeredConnectionsReader(std::unique_ptr<IProcessConnectionsReader> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasConnections() const override;
    [[nodiscard]] ConnectionsReadResult readConnections(const ProcessTarget& target) override;
    [[nodiscard]] bool canReadAuthorized() const override;
    [[nodiscard]] ConnectionsReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken) override;

  private:
    [[nodiscard]] ConnectionsReadResult viaBroker(const ProcessTarget& target, const std::stop_token& stopToken);

    std::unique_ptr<IProcessConnectionsReader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' threads list that an administrator can finish (#3069), as BrokeredConnectionsReader.
class BrokeredThreadsReader final : public IProcessThreadsReader
{
  public:
    BrokeredThreadsReader(std::unique_ptr<IProcessThreadsReader> inner, std::shared_ptr<IPrivilegedBroker> broker);

    [[nodiscard]] bool hasThreads() const override;
    [[nodiscard]] ThreadsReadResult readThreads(const ProcessTarget& target) override;
    [[nodiscard]] bool canReadAuthorized() const override;
    [[nodiscard]] ThreadsReadResult readAuthorized(const ProcessTarget& target, const std::stop_token& stopToken) override;

  private:
    [[nodiscard]] ThreadsReadResult viaBroker(const ProcessTarget& target, const std::stop_token& stopToken);

    std::unique_ptr<IProcessThreadsReader> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// @p reader decorated with @p broker, or @p reader as it is when there is no broker.
[[nodiscard]] std::unique_ptr<IProcessEnvironmentReader> withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader> reader,
                                                                              std::shared_ptr<IPrivilegedBroker> broker);
[[nodiscard]] std::unique_ptr<IProcessModulesReader> withPrivilegedBroker(std::unique_ptr<IProcessModulesReader> reader,
                                                                          std::shared_ptr<IPrivilegedBroker> broker);
[[nodiscard]] std::unique_ptr<IProcessOpenFilesReader> withPrivilegedBroker(std::unique_ptr<IProcessOpenFilesReader> reader,
                                                                            std::shared_ptr<IPrivilegedBroker> broker);

[[nodiscard]] std::unique_ptr<IProcessConnectionsReader> withPrivilegedBroker(std::unique_ptr<IProcessConnectionsReader> reader,
                                                                              std::shared_ptr<IPrivilegedBroker> broker);
[[nodiscard]] std::unique_ptr<IProcessThreadsReader> withPrivilegedBroker(std::unique_ptr<IProcessThreadsReader> reader,
                                                                          std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
