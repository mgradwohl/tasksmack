#pragma once

#include "Platform/CpuAffinity.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace Platform
{

/// Result of a process action (kill, terminate, etc.)
struct ProcessActionResult
{
    bool success = false;
    std::string errorMessage;
    /// On success, how the action was carried out when that differs from what its name promises, as a
    /// phrase the result line appends: Windows affinity across processor groups is only a preference
    /// (#2173). Empty for a plain success and on failure.
    std::string successNote;
    /// On failure, whether the OS refused it for want of permission over the process (Linux: EPERM or
    /// EACCES), which acting as an administrator could cure: BrokeredProcessActions retries such a
    /// failure through the privileged helper (#2308). False for every other failure.
    bool permissionDenied = false;

    static ProcessActionResult ok(std::string note = {})
    {
        return {.success = true, .errorMessage = {}, .successNote = std::move(note), .permissionDenied = false};
    }
    static ProcessActionResult error(std::string msg)
    {
        return {.success = false, .errorMessage = std::move(msg), .successNote = {}, .permissionDenied = false};
    }
    /// A failure for want of permission over the process (permissionDenied).
    static ProcessActionResult denied(std::string msg)
    {
        return {.success = false, .errorMessage = std::move(msg), .successNote = {}, .permissionDenied = true};
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

    /// The same process: the same PID and the same start time. The Processes table's selection is
    /// keyed on this exact identity, never on a hash of it (#1503).
    [[nodiscard]] friend constexpr bool operator==(const ProcessTarget&, const ProcessTarget&) noexcept = default;
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
/// Whether IProcessActions::launchSyscallTrace() can open a system call tracer on this machine (#182).
enum class SyscallTraceAvailability : std::uint8_t
{
    Unsupported, ///< No such action here (Windows, or the synthetic scenario): the UI hides it.
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
        // Windows, and the synthetic scenario on any platform.
        return "Tracing system calls is not available here";
    case SyscallTraceAvailability::NoTracer:
        return "strace is not installed (or not on PATH). Install it (e.g. sudo apt install strace) and restart TaskSmack.";
    case SyscallTraceAvailability::NoTerminal:
        return "No terminal emulator was found to run strace in. Set $TERMINAL to one (a program name or absolute path), or "
               "install one such as gnome-terminal, konsole or xterm, and restart TaskSmack.";
    case SyscallTraceAvailability::Available:
        break;
    }
    return "";
}

/// How the machine's logical processors are grouped, for the Set affinity dialog (#1881): read on
/// demand, when the dialog opens, never per frame. Each part may be empty, meaning unknown; the dialog
/// then offers CPUs 0 to N-1 in one plain grid.
struct CpuAffinityLayout
{
    /// Windows processor groups' sizes (their maximum processor counts), in CPU order: group 0's CPUs
    /// come first, numbered as CpuAffinity numbers them. Empty (Linux) or one entry: no groups to show.
    std::vector<std::uint32_t> groupSizes;
    /// The CPUs that exist now, numbered as CpuAffinity numbers them; empty when unknown.
    CpuAffinity online;
    /// Each CPU's efficiency class on a hybrid CPU (higher is more performant), as
    /// CpuDetails::efficiencyClassByCoreId has it; empty on a CPU with one kind of core.
    std::vector<std::uint8_t> efficiencyClassByCpu;
    /// setAffinity() sets the choice as the process's default CPU sets rather than a hard affinity:
    /// Windows with more than one processor group, where one affinity mask can't span groups.
    bool appliedAsCpuSets = false;
};

/// A dump file IProcessActions::createDump() wrote (#807).
struct ProcessDump
{
    std::filesystem::path file;  ///< The file written.
    std::uint64_t sizeBytes = 0; ///< Its size on disk.
    std::string note;            ///< Something worth knowing about the dump (a 32-bit process's); empty when nothing.
    /// Written from a snapshot of the process (Windows: PssCaptureSnapshot's clone, #2001), so the
    /// process paused only while the snapshot was taken, not for the whole write.
    bool fromSnapshot = false;
    /// Why it was written from the running process instead, or, where the platform never takes a
    /// snapshot (ProcessActionCapabilities::dumpsFromSnapshot false), how; empty when fromSnapshot.
    std::string liveReason;
};

/// The dump file written, or the message saying why there is none.
using ProcessDumpResult = std::expected<ProcessDump, std::string>;

/// What createDump() says where the platform cannot write a dump.
inline constexpr const char* DUMP_UNSUPPORTED_MESSAGE = "Creating a dump file isn't available on this system";

/// What a dump's free-space check is made from (#2001): raw readings, each empty when it couldn't be
/// read. The estimate and the warning are the UI's (App::DumpDetail).
struct DumpSpace
{
    /// Bytes free to TaskSmack's user on the volume holding the folder.
    std::optional<std::uint64_t> freeBytes;
    /// The process's private bytes (its commit charge): a full dump holds at least that much.
    std::optional<std::uint64_t> processBytes;
};

/// The folder chosen in IProcessActions::chooseDumpFolder(), nullopt when the dialog was cancelled, or
/// the message saying why it couldn't be shown.
using FolderChoice = std::expected<std::optional<std::filesystem::path>, std::string>;

/// What chooseDumpFolder() says where the platform has no folder dialog.
inline constexpr const char* FOLDER_CHOICE_UNSUPPORTED_MESSAGE = "Choosing a folder isn't available on this system";

/// An action to retry as an administrator (IProcessActions::runAuthorized(), #2308): which one, its
/// target, and its argument where it has one.
/// The signals beyond terminate, kill, suspend and resume that TaskSmack can send (#3063): what reloads a
/// daemon (Hangup), interrupts a tool as Ctrl+C does (Interrupt), asks for a core dump (Quit), or triggers
/// a program's own handler (User1, User2). Linux only (ProcessActionCapabilities::canSendSignals).
enum class ProcessSignal : std::uint8_t
{
    Hangup,    ///< SIGHUP
    Interrupt, ///< SIGINT
    Quit,      ///< SIGQUIT
    User1,     ///< SIGUSR1
    User2,     ///< SIGUSR2
};

struct AuthorizedAction
{
    enum class Kind : std::uint8_t
    {
        Terminate,
        Kill,
        Stop,
        Resume,
        SetPriority,   ///< nice
        SetIoPriority, ///< ioClass and level
        SetAffinity,   ///< cpus
        SendSignal,    ///< signal (#3063)
    };
    Kind kind = Kind::Terminate;
    ProcessTarget target;
    ProcessSignal signal = ProcessSignal::Hangup;
    std::int32_t nice = 0;
    IoPriorityClass ioClass = IoPriorityClass::BestEffort;
    std::int32_t level = 4;
    // NOLINTNEXTLINE(readability-redundant-member-init) - {} lets designated initializers omit it without a warning
    CpuAffinity cpus{};
};

/// Run new task (#3065): a program to start, as Task Manager's File > Run new task.
struct RunProgramRequest
{
    std::string commandLine;      ///< The program and its arguments, split as CommandWords::split() does
    bool asAdministrator = false; ///< Started with administrator rights (Linux: through pkexec)
};

/// Capabilities for process actions.
struct ProcessActionCapabilities
{
    bool canTerminate = false;     // SIGTERM
    bool canKill = false;          // SIGKILL
    bool canStop = false;          // SIGSTOP
    bool canContinue = false;      // SIGCONT
    bool canSendSignals = false;   // sendSignal(): HUP, INT, QUIT, USR1, USR2 (Linux, #3063)
    bool canSetPriority = false;   // setpriority/SetPriorityClass
    bool canSetIoPriority = false; // ioprio_set (Linux only)
    /// The Realtime I/O class too: ioprio_set(IOPRIO_CLASS_RT) needs CAP_SYS_NICE (or CAP_SYS_ADMIN) in
    /// TaskSmack's own effective set, read once at construction (Linux only, #1540).
    bool canSetRealtimeIoPriority = false;
    /// setAffinity(): which processors the process may run on (#1887 Linux: sched_setaffinity;
    /// #1881 Windows).
    bool canSetAffinity = false;
    /// launchSyscallTrace(): found once, when the implementation is constructed, never per frame.
    SyscallTraceAvailability syscallTrace = SyscallTraceAvailability::Unsupported;
    /// createDump(): a full-memory dump file of the process (#807; Windows: MiniDumpWriteDump).
    bool canCreateDump = false;
    /// chooseDumpFolder(): a folder dialog for where dumps go (#2001; Windows: IFileOpenDialog).
    bool canChooseDumpFolder = false;
    /// createDump() first tries a snapshot of the process, so it pauses only while that is taken, and
    /// writes from the running process only when none can be (Windows: PssCaptureSnapshot, #2001).
    /// False: every dump is written from the running process, stopped for the whole write (Linux:
    /// gdb's gcore), and that is the normal case, not a fallback (#2241).
    bool dumpsFromSnapshot = false;
    /// runAuthorized() can retry a permissionDenied failure as an administrator, after a password
    /// prompt (Linux: the pkexec-started helper, #2308). The UI then offers "Authenticate and retry".
    bool canAuthorizeInteractively = false;
    /// openFileLocation(): the file manager can be asked to show a process's executable (#3064; Linux:
    /// FileManager1 over D-Bus or xdg-open).
    bool canOpenFileLocation = false;
    /// runProgram(): Run new task can start a program (#3065), and with administrator rights too where
    /// canRunProgramAsAdministrator says so (Linux: pkexec is installed, or TaskSmack runs as root).
    bool canRunProgram = false;
    bool canRunProgramAsAdministrator = false;
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

    /// What actions this platform supports. Cheap and non-blocking: Process Details asks every frame, on
    /// the UI thread, so it reports what was found at construction and never reads a file, asks the
    /// system for a list, or looks at a helper's channel (#3182).
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
    /// Open a terminal window running a system call tracer (strace) attached to the target (#182).
    ///
    /// Returns once the terminal has been started, never waiting for it or the tracer to finish; the
    /// trace itself is shown and ended in that window. Like every action it refuses a target whose
    /// identity cannot be confirmed. Only LinuxProcessActions implements it. The default refuses it as
    /// unsupported, matching ProcessActionCapabilities' default syscallTrace (Unsupported), which an
    /// implementation that does not override this must leave as it is.
    [[nodiscard]] virtual ProcessActionResult launchSyscallTrace(const ProcessTarget& target)
    {
        static_cast<void>(target);
        return ProcessActionResult::error(syscallTraceUnavailableReason(SyscallTraceAvailability::Unsupported));
    }

    /// Opens the file manager on the target's executable, selected where the file manager can do that
    /// (#3064), as Task Manager's "Open file location". Returns once the file manager has been asked,
    /// never waiting for it. Refused, as every action is, unless the process at the target's PID is the
    /// target. The default refuses it as unsupported, matching
    /// ProcessActionCapabilities::canOpenFileLocation's default (false), which an implementation that
    /// does not override this must leave as it is.
    [[nodiscard]] virtual ProcessActionResult openFileLocation(const ProcessTarget& target)
    {
        static_cast<void>(target);
        return ProcessActionResult::error("Opening a file location isn't available on this system");
    }

    /// Starts the program @p request names, detached from TaskSmack (#3065), and returns once it has been
    /// started, never waiting for it. Its first word is looked up on PATH unless it holds a '/'. The
    /// default refuses it as unsupported, matching ProcessActionCapabilities::canRunProgram's default
    /// (false), which an implementation that does not override this must leave as it is.
    [[nodiscard]] virtual ProcessActionResult runProgram(const RunProgramRequest& request)
    {
        static_cast<void>(request);
        return ProcessActionResult::error("Running a program isn't available on this system");
    }

    /// Lets the target run only on @p cpus (#1887), numbered as CpuAffinity and the per-core figures
    /// number them; refused when @p cpus is empty. Refused, as every action is, unless the process at
    /// the target's PID is the target. Where the choice is applied as a preference rather than a hard
    /// affinity (Windows CPU sets, #2173), the success carries a ProcessActionResult::successNote that
    /// says so. The default refuses it as unsupported, matching
    /// ProcessActionCapabilities::canSetAffinity's default (false), which an implementation that does
    /// not override this must leave as it is.
    [[nodiscard]] virtual ProcessActionResult setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
    {
        static_cast<void>(target);
        static_cast<void>(cpus);
        return ProcessActionResult::error("Setting CPU affinity isn't available on this system");
    }

    /// Writes a full-memory dump of the target into @p directory (#807), as Task Manager's "Create dump
    /// file" does, and returns the file written: from a snapshot of the process where the platform can
    /// take one (ProcessDump::fromSnapshot), else from the running process. Blocking, for seconds or minutes with a large process,
    /// so it is called on a worker thread, never the UI thread; it may run there while the UI thread
    /// calls the other actions, so an implementation keeps no state that this changes. Once
    /// @p stopToken is requested it stops at its next chance and deletes the partial file. Refused, as
    /// every action is, unless the process at the target's PID is the target. The default refuses it
    /// as unsupported, matching ProcessActionCapabilities::canCreateDump's default (false), which an
    /// implementation that does not override this must leave as it is.
    [[nodiscard]] virtual ProcessDumpResult
    createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
    {
        static_cast<void>(target);
        static_cast<void>(directory);
        static_cast<void>(stopToken);
        return std::unexpected(std::string(DUMP_UNSUPPORTED_MESSAGE));
    }

    /// The free space on @p directory's volume and the target's private bytes, for the dump's confirm
    /// to warn when the dump may not fit (#2001). Reads the system each call: call it when the confirm
    /// opens or its folder changes, not per frame. The default knows neither.
    [[nodiscard]] virtual DumpSpace dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
    {
        static_cast<void>(target);
        static_cast<void>(directory);
        return {};
    }

    /// Shows the platform's folder dialog, starting in @p initial, for where dump files go (#2001), and
    /// returns the folder chosen (nullopt: cancelled). Blocks until the user answers, so it is called
    /// on a worker thread, never the UI thread. @p ownerWindow is the native window the dialog is
    /// modal to (an HWND on Windows), or null. The default refuses it as unsupported, matching
    /// ProcessActionCapabilities::canChooseDumpFolder's default (false).
    [[nodiscard]] virtual FolderChoice chooseDumpFolder(const std::filesystem::path& initial, void* ownerWindow)
    {
        static_cast<void>(initial);
        static_cast<void>(ownerWindow);
        return std::unexpected(std::string(FOLDER_CHOICE_UNSUPPORTED_MESSAGE));
    }

    /// Sends @p signal to the process (#3063), with the same identity check and refusals as terminate():
    /// never to init or a kernel thread, and never to a reused PID. The default refuses it, matching
    /// canSendSignals' default (false).
    [[nodiscard]] virtual ProcessActionResult sendSignal(const ProcessTarget& target, ProcessSignal signal)
    {
        static_cast<void>(target);
        static_cast<void>(signal);
        return ProcessActionResult::error("Sending this signal isn't available on this system");
    }

    /// Retries @p request (a signal, priority, I/O priority or affinity change) as an administrator,
    /// asking for the password first if need be (#2308): what "Authenticate and retry" does after a
    /// permissionDenied failure. Blocks for as long as the prompt is up, so it runs on a worker, never
    /// the UI thread. The default refuses it, matching ProcessActionCapabilities'
    /// canAuthorizeInteractively default (false).
    [[nodiscard]] virtual ProcessActionResult runAuthorized(const AuthorizedAction& action, const std::stop_token& stopToken)
    {
        static_cast<void>(action);
        static_cast<void>(stopToken);
        return ProcessActionResult::error("Acting as an administrator isn't available");
    }

    /// The processor groups, the CPUs online and the core kinds, for the Set affinity dialog (#1881).
    /// Reads the system each call (GetLogicalProcessorInformationEx, sysfs), so it is called on a worker,
    /// once, and kept: the layout doesn't change while TaskSmack runs (#3182). Never on the UI thread. The
    /// default knows nothing (every part empty).
    [[nodiscard]] virtual CpuAffinityLayout affinityLayout() const
    {
        return {};
    }
};

} // namespace Platform
