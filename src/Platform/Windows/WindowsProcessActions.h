#pragma once

#include "Platform/IProcessActions.h"

namespace Platform
{

/// Windows implementation of IProcessActions.
/// Uses TerminateProcess and related APIs. Each action opens a handle to the PID, confirms the
/// process's creation time matches the target's, and acts through that handle. The handle pins the
/// process object, so once the check passes a later reuse of the PID cannot redirect the action.
class WindowsProcessActions : public IProcessActions
{
  public:
    WindowsProcessActions() = default;
    ~WindowsProcessActions() override = default;

    WindowsProcessActions(const WindowsProcessActions&) = delete;
    WindowsProcessActions& operator=(const WindowsProcessActions&) = delete;
    WindowsProcessActions(WindowsProcessActions&&) = default;
    WindowsProcessActions& operator=(WindowsProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;
    /// Windows has no ionice equivalent (#803): refused, and canSetIoPriority is false.
    [[nodiscard]] ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) override;
    /// Refused, as setIoPriority() is.
    [[nodiscard]] IoPriorityReadResult getIoPriority(const ProcessTarget& target) override;

  private:
    /// Helper to terminate a process with given exit code
    [[nodiscard]] static ProcessActionResult terminateProcess(const ProcessTarget& target, uint32_t exitCode);
};

} // namespace Platform
