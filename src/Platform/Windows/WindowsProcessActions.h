#pragma once

#include "Platform/IProcessActions.h"

namespace Platform
{

/// Windows implementation of IProcessActions.
/// Uses TerminateProcess and related APIs.
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
    [[nodiscard]] ProcessActionResult terminate(int32_t pid) override;
    [[nodiscard]] ProcessActionResult kill(int32_t pid) override;
    [[nodiscard]] ProcessActionResult stop(int32_t pid) override;
    [[nodiscard]] ProcessActionResult resume(int32_t pid) override;

  private:
    /// Helper to terminate a process with given exit code
    [[nodiscard]] static ProcessActionResult terminateProcess(int32_t pid, uint32_t exitCode);
};

} // namespace Platform
