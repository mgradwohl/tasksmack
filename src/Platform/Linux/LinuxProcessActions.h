#pragma once

#include "Platform/IProcessActions.h"

#include <string_view>

namespace Platform
{

/// Linux implementation of IProcessActions.
/// Signals go through a pidfd: the action opens one for the PID, confirms the start time in
/// /proc/[pid]/stat matches the target's, and signals through the pidfd, which keeps referring to
/// the process it was opened for even if the PID is later reused. setpriority(2) has no pidfd form,
/// so it is aimed at the PID after the check, and the pidfd is asked afterwards whether the target
/// survived the call: if it did, its PID never changed hands. Where pidfds are unavailable (old
/// kernels, some sandboxes) both paths fall back to acting on the PID straight after the check.
class LinuxProcessActions : public IProcessActions
{
  public:
    LinuxProcessActions() = default;
    ~LinuxProcessActions() override = default;

    LinuxProcessActions(const LinuxProcessActions&) = delete;
    LinuxProcessActions& operator=(const LinuxProcessActions&) = delete;
    LinuxProcessActions(LinuxProcessActions&&) = default;
    LinuxProcessActions& operator=(LinuxProcessActions&&) = default;

    [[nodiscard]] ProcessActionCapabilities actionCapabilities() const override;
    [[nodiscard]] ProcessActionResult terminate(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult kill(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult stop(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult resume(const ProcessTarget& target) override;
    [[nodiscard]] ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) override;

  private:
    [[nodiscard]] static ProcessActionResult sendSignal(const ProcessTarget& target, int signal, std::string_view signalName);
};

} // namespace Platform
