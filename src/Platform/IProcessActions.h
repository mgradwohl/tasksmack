#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace Platform
{

/// Result of a process action (kill, terminate, etc.)
struct ProcessActionResult
{
    bool success = false;
    std::string errorMessage;

    static ProcessActionResult ok()
    {
        return {.success = true, .errorMessage = {}};
    }
    static ProcessActionResult error(std::string msg)
    {
        return {.success = false, .errorMessage = std::move(msg)};
    }
};

/// The process an action is meant for: its PID, and the start time that tells it apart from any
/// later process given the same PID.
///
/// A PID alone names a process only while that process is alive. Once it exits the system may hand
/// the PID to a new process, and an action dispatched by PID alone would land on that one instead
/// (#973). The start time is the same raw value the probe reports in
/// ProcessCounters::startTimeTicks -- creation time in 100ns FILETIME units on Windows, clock ticks
/// since boot from /proc/[pid]/stat on Linux -- so the platform can compare it against the process
/// it is about to act on.
struct ProcessTarget
{
    std::int32_t pid = 0;
    std::uint64_t startTimeTicks = 0; ///< 0 means unknown; an action on an unknown identity is refused.
};

/// Whether the process found at a target's PID is the process the target names.
///
/// Refuses an unknown expected start time rather than skipping the check: acting on a PID that cannot
/// be confirmed is exactly the exposure this exists to close. Only kernel pseudo-processes (Windows'
/// Idle and System) report no start time, and those cannot be acted on anyway.
///
/// @param target            The process the caller meant.
/// @param actualStartTicks  Start time of the process holding the PID now, read by the platform.
/// @return ok() when they match; otherwise an error the UI can show as it stands.
[[nodiscard]] inline ProcessActionResult checkProcessIdentity(const ProcessTarget& target, std::uint64_t actualStartTicks)
{
    if (target.startTimeTicks == 0)
    {
        return ProcessActionResult::error(std::format("Cannot confirm the identity of process {}; action not sent", target.pid));
    }
    if (actualStartTicks != target.startTimeTicks)
    {
        return ProcessActionResult::error(
            std::format("Process {} has exited and its PID now belongs to a different process; action not sent", target.pid));
    }
    return ProcessActionResult::ok();
}

/// Whether IProcessActions::launchSyscallTrace() can open a system call tracer on this machine (#182).
enum class SyscallTraceAvailability : std::uint8_t
{
    Unsupported, ///< The platform has no such action (Windows): the UI hides it.
    Available,   ///< A tracer and a terminal to show it in were found.
    NoTracer,    ///< The platform supports it, but no tracer (strace) was found on PATH.
    NoTerminal,  ///< The platform supports it, but no terminal emulator was found to run it in.
};

/// Why the trace action cannot run, for the tooltip of its disabled button; empty when it can.
[[nodiscard]] constexpr const char* syscallTraceUnavailableReason(SyscallTraceAvailability availability) noexcept
{
    switch (availability)
    {
    case SyscallTraceAvailability::Unsupported:
        return "Tracing system calls is not supported on this platform";
    case SyscallTraceAvailability::NoTracer:
        return "strace is not installed (or not on PATH). Install it (e.g. sudo apt install strace) and restart TaskSmack.";
    case SyscallTraceAvailability::NoTerminal:
        return "No terminal emulator was found to run strace in. Set $TERMINAL, or install x-terminal-emulator, gnome-terminal, "
               "konsole or xterm, and restart TaskSmack.";
    case SyscallTraceAvailability::Available:
        break;
    }
    return "";
}

/// Capabilities for process actions.
struct ProcessActionCapabilities
{
    bool canTerminate = false;   // SIGTERM
    bool canKill = false;        // SIGKILL
    bool canStop = false;        // SIGSTOP
    bool canContinue = false;    // SIGCONT
    bool canSetPriority = false; // setpriority/SetPriorityClass
    /// launchSyscallTrace(): found once, when the implementation is constructed, never per frame.
    SyscallTraceAvailability syscallTrace = SyscallTraceAvailability::Unsupported;
};

/// Interface for platform-specific process actions.
///
/// Every action takes a ProcessTarget rather than a bare PID, and an implementation must refuse the
/// action unless checkProcessIdentity() accepts the process it would act on.
class IProcessActions
{
  public:
    virtual ~IProcessActions() = default;

    IProcessActions() = default;
    IProcessActions(const IProcessActions&) = default;
    IProcessActions& operator=(const IProcessActions&) = default;
    IProcessActions(IProcessActions&&) = default;
    IProcessActions& operator=(IProcessActions&&) = default;

    /// What actions this platform supports.
    [[nodiscard]] virtual ProcessActionCapabilities actionCapabilities() const = 0;

    /// Send SIGTERM (graceful termination request).
    [[nodiscard]] virtual ProcessActionResult terminate(const ProcessTarget& target) = 0;

    /// Send SIGKILL (forceful kill).
    [[nodiscard]] virtual ProcessActionResult kill(const ProcessTarget& target) = 0;

    /// Send SIGSTOP (pause process).
    [[nodiscard]] virtual ProcessActionResult stop(const ProcessTarget& target) = 0;

    /// Send SIGCONT (resume paused process).
    [[nodiscard]] virtual ProcessActionResult resume(const ProcessTarget& target) = 0;

    /// Set process priority (nice value on Unix, priority class on Windows).
    /// @param target Process to change
    /// @param nice Nice value (-20 to 19 on Unix, mapped to priority class on Windows)
    [[nodiscard]] virtual ProcessActionResult setPriority(const ProcessTarget& target, int32_t nice) = 0;

    /// Open a terminal window running a system call tracer (strace) attached to the target (#182).
    ///
    /// Returns once the terminal has been started, never waiting for it or the tracer to finish; the
    /// trace itself is shown and ended in that window. Like every action it refuses a target whose
    /// identity cannot be confirmed. Only Linux implements it: the default reports it unsupported, as
    /// actionCapabilities().syscallTrace does (SyscallTraceAvailability::Unsupported).
    [[nodiscard]] virtual ProcessActionResult launchSyscallTrace(const ProcessTarget& target)
    {
        static_cast<void>(target);
        return ProcessActionResult::error(syscallTraceUnavailableReason(SyscallTraceAvailability::Unsupported));
    }
};

} // namespace Platform
