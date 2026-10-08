#pragma once

#include <cstdint>
#include <expected>
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

/// A Linux I/O scheduling class (ioprio_set(2)), with the kernel's own numbering (IOPRIO_CLASS_*).
enum class IoPriorityClass : std::uint8_t
{
    None = 0,       ///< Never set: the kernel derives a best-effort level from the nice value.
    Realtime = 1,   ///< Served before every other class; setting it needs CAP_SYS_NICE (or CAP_SYS_ADMIN).
    BestEffort = 2, ///< The normal class, with levels 0 (highest) to 7 (lowest).
    Idle = 3,       ///< Served only when no other process wants the disk.
};

/// A process's I/O priority: its class, and its level within Realtime or BestEffort (0 is highest,
/// 7 lowest). The level means nothing for None and Idle, and is 0 there.
struct IoPriority
{
    IoPriorityClass ioClass = IoPriorityClass::None;
    std::int32_t level = 0;

    [[nodiscard]] constexpr bool operator==(const IoPriority&) const noexcept = default;
};

/// The I/O priority read for a process, or the message saying why it could not be read.
using IoPriorityReadResult = std::expected<IoPriority, std::string>;

/// Capabilities for process actions.
struct ProcessActionCapabilities
{
    bool canTerminate = false;     // SIGTERM
    bool canKill = false;          // SIGKILL
    bool canStop = false;          // SIGSTOP
    bool canContinue = false;      // SIGCONT
    bool canSetPriority = false;   // setpriority/SetPriorityClass
    bool canSetIoPriority = false; // ioprio_set (Linux only)
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

    /// Set the process's I/O priority (ioprio_set on Linux, for every thread). Refused where
    /// actionCapabilities().canSetIoPriority is false.
    /// @param target  Process to change
    /// @param ioClass Scheduling class
    /// @param level   Level within Realtime or BestEffort (0 highest to 7 lowest); ignored otherwise
    [[nodiscard]] virtual ProcessActionResult setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level) = 0;

    /// Read the process's current I/O priority (ioprio_get on Linux), on demand for one process: it
    /// is not part of the sampled counters. Refused, as the actions are, unless the process at the
    /// target's PID is the target.
    [[nodiscard]] virtual IoPriorityReadResult getIoPriority(const ProcessTarget& target) = 0;
};

} // namespace Platform
