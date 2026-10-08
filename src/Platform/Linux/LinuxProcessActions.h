#pragma once

#include "Platform/IProcessActions.h"
#include "SyscallTrace.h"

#include <string_view>

namespace Platform
{

/// Linux implementation of IProcessActions.
/// Signals go through a pidfd: the action opens one for the PID, confirms the start time in
/// /proc/[pid]/stat matches the target's, and signals through the pidfd, which keeps referring to
/// the process it was opened for even if the PID is later reused. setpriority(2) has no pidfd form,
/// so it is aimed at the PID after the check, and the pidfd is asked afterwards whether the target
/// survived the call: if it did, its PID never changed hands. Where pidfds are unavailable (kernels
/// before 5.3, or a seccomp filter that blocks pidfd_open) every action is refused rather than sent
/// to the bare PID.
///
/// launchSyscallTrace() opens a terminal running `strace -p` (#182). strace and the terminal are looked
/// up once, at construction (discoverSyscallTraceTools()), and actionCapabilities() reports what was
/// found; installing either later takes effect on the next start.
class LinuxProcessActions : public IProcessActions
{
  public:
    /// Finds strace and a terminal emulator on PATH (and $TERMINAL) for launchSyscallTrace().
    LinuxProcessActions();
    /// Uses @p traceTools instead of searching (tests: a fake terminal that records its argv).
    explicit LinuxProcessActions(SyscallTrace::Tools traceTools);
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
    [[nodiscard]] ProcessActionResult launchSyscallTrace(const ProcessTarget& target) override;

    /// strace and the terminal to run it in, from this process's PATH and $TERMINAL.
    [[nodiscard]] static SyscallTrace::Tools discoverSyscallTraceTools();

  private:
    [[nodiscard]] static ProcessActionResult sendSignal(const ProcessTarget& target, int signal, std::string_view signalName);

    SyscallTrace::Tools m_TraceTools;
};

} // namespace Platform
