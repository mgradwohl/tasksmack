#pragma once

// Process actions that an administrator can finish (#2308; #2181): an IProcessActions decorator over the
// in-process implementation and the privileged broker (IPrivilegedBroker: the pkexec-started helper on
// Linux, #2307). Each action is tried in-process first, as before. Only a failure the OS gave for want of
// permission (ProcessActionResult::permissionDenied) goes further:
//
// - while the helper is running (the user authenticated a few minutes ago), the action is retried through
//   it at once, with no prompt;
// - otherwise the refusal is returned as it is, and the UI offers "Authenticate and retry"
//   (actionCapabilities().canAuthorizeInteractively), which calls runAuthorized(): that asks for the
//   password, starts the helper, and sends the action through it.
//
// The helper re-checks every target by PID and start time as root, so a retried action never reaches a
// process other than the one the in-process attempt was for. A failure from the helper (running as root)
// is never permissionDenied, so nothing is retried twice. App composes one per IProcessActions, all over
// the one broker, and only where the helper is installed and TaskSmack isn't already an administrator.

#include "Platform/BrokerProtocol.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessEnvironment.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stop_token>

namespace Platform
{

/// @p actions decorated with @p broker (BrokeredProcessActions), or @p actions as they are when there is
/// no broker.
[[nodiscard]] std::unique_ptr<IProcessActions> withPrivilegedBroker(std::unique_ptr<IProcessActions> actions,
                                                                    std::shared_ptr<IPrivilegedBroker> broker);

/// The broker request that does @p action.
[[nodiscard]] Broker::Request brokerRequestFor(const AuthorizedAction& action);

class BrokeredProcessActions final : public IProcessActions
{
  public:
    BrokeredProcessActions(std::unique_ptr<IProcessActions> inner, std::shared_ptr<IPrivilegedBroker> broker);

    /// The in-process capabilities, plus canAuthorizeInteractively; and the Realtime I/O class while
    /// the helper runs, which can set it.
    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;

    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, std::int32_t nice) override;
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, std::int32_t level) override;
    [[nodiscard]] ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus) override;

    /// Asks for the password if the helper isn't running, then does @p action through it.
    [[nodiscard]] ProcessActionResult runAuthorized(const AuthorizedAction& action, const std::stop_token& stopToken) override;

    // Not actions an administrator finishes: the in-process implementation's own.
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult launchSyscallTrace(const ProcessTarget& target) override;
    [[nodiscard]] ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken) override;
    [[nodiscard]] DumpSpace dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory) override;
    [[nodiscard]] FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow) override;
    [[nodiscard]] CpuAffinityLayout affinityLayout() const override;

  private:
    /// @p result, or, when it is a permission refusal and the helper is running, @p action's result
    /// through the helper.
    [[nodiscard]] ProcessActionResult retried(ProcessActionResult result, const AuthorizedAction& action);

    std::unique_ptr<IProcessActions> m_Inner;
    std::shared_ptr<IPrivilegedBroker> m_Broker;
};

/// Process Details' environment read that an administrator can finish (#2309), as
/// BrokeredProcessActions does for the actions: read in-process first; a PermissionDenied read goes
/// through the helper at once while it runs, and otherwise is returned for the UI to offer
/// "Authenticate to read" (readAuthorized()). The helper reads as root, with the same identity check.
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

/// @p reader decorated with @p broker, or @p reader as it is when there is no broker.
[[nodiscard]] std::unique_ptr<IProcessEnvironmentReader> withPrivilegedBroker(std::unique_ptr<IProcessEnvironmentReader> reader,
                                                                              std::shared_ptr<IPrivilegedBroker> broker);

} // namespace Platform
