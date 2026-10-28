#include "LinuxProcessControl.h"

#include "Domain/PriorityConfig.h"
#include "IoPriority.h"
#include "LinuxCpuDetails.h"
#include "LinuxProcessActionsDetail.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "PosixGuards.h"
#include "PriorityErrorMessage.h"
#include "ProcParsing.h"
#include "ProcPrivileges.h"
#include "ThreadPriority.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides SIGKILL, SIGSTOP and SIGCONT; csignal is ISO C's
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// What the actions need of whatever process holds `pid` right now: its start time and kernel flags.
struct StatIdentity
{
    std::uint64_t startTime = 0;
    std::uint32_t flags = 0;
};

/// StatIdentity of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<StatIdentity> readStatIdentity(std::int32_t pid)
{
    const std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const FdGuard fd(::open(statPath.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::nullopt;
    }

    // 1 KiB is ample: comm is kernel-capped at 15 chars and starttime is the 22nd field.
    std::array<char, 1024> buf{};
    const auto len = ::read(fd.get(), buf.data(), buf.size());
    if (len <= 0)
    {
        return std::nullopt;
    }
    const std::optional<ProcParsing::StatFields> fields =
        ProcParsing::parseStatFields(std::string_view(buf.data(), static_cast<std::size_t>(len)));
    if (!fields.has_value())
    {
        return std::nullopt;
    }
    return StatIdentity{.startTime = fields->startTime, .flags = fields->flags};
}

/// Start time of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<std::uint64_t> readStartTicks(std::int32_t pid)
{
    const std::optional<StatIdentity> stat = readStatIdentity(pid);
    return stat.has_value() ? std::optional<std::uint64_t>(stat->startTime) : std::nullopt;
}

/// A process TaskSmack and the root helper don't act on, whatever the action (#2447, #2641, #2774).
enum class ProtectedProcess : std::uint8_t
{
    Init,         ///< PID 1
    KernelThread, ///< PF_KTHREAD in /proc/<pid>/stat
};

/// Whether `pid` is one TaskSmack doesn't act on, and why. The one rule both guards below word their
/// own messages for, so signals and scheduling changes always refuse the same processes.
[[nodiscard]] std::optional<ProtectedProcess> protectedProcess(std::int32_t pid)
{
    if (pid == 1)
    {
        return ProtectedProcess::Init;
    }
    const std::optional<StatIdentity> stat = readStatIdentity(pid);
    if (stat.has_value() && (stat->flags & ProcParsing::PF_KTHREAD_FLAG) != 0)
    {
        return ProtectedProcess::KernelThread;
    }
    return std::nullopt;
}

/// Why a signal to `pid` would do nothing, or nullopt. As root (the helper, #2308) the kernel accepts a
/// signal to a kernel thread or to init and drops it, so TaskSmack would report an action that never
/// happened (#2447): kernel threads ignore signals, and init drops SIGKILL and SIGSTOP and anything it
/// has no handler for (systemd re-executes itself on SIGTERM). An error, not a refusal: no password helps.
[[nodiscard]] std::optional<std::string> signalWouldDoNothing(std::int32_t pid)
{
    const std::optional<ProtectedProcess> reason = protectedProcess(pid);
    if (!reason.has_value())
    {
        return std::nullopt;
    }
    return *reason == ProtectedProcess::Init ? std::string("PID 1 is init, which TaskSmack doesn't end, suspend or resume")
                                             : std::format("PID {} is a kernel thread, which can't be ended, suspended or resumed", pid);
}

/// Why a priority, I/O priority or affinity change is refused before it's tried (#2641), as a signal is
/// (signalWouldDoNothing()): PID 1, whose priority and affinity every service systemd starts inherits,
/// and kernel threads (kswapd, jbd2/*, kworker/u*), where a low priority or the Idle I/O class can stall
/// reclaim or the journal for the whole machine. Through the helper they would otherwise succeed.
[[nodiscard]] std::optional<std::string> schedulingChangeRefused(std::int32_t pid)
{
    const std::optional<ProtectedProcess> reason = protectedProcess(pid);
    if (!reason.has_value())
    {
        return std::nullopt;
    }
    return *reason == ProtectedProcess::Init
             ? std::string("PID 1 is init, whose priority and CPU affinity TaskSmack doesn't change")
             : std::format("PID {} is a kernel thread, whose priority and CPU affinity TaskSmack doesn't change", pid);
}

/// Error text shared by every action refused because pidfds are unavailable.
[[nodiscard]] std::string noPidfdMessage(std::int32_t pid)
{
    return std::format("Cannot act on process {} safely: this system does not provide pidfds (Linux 5.3 or later, "
                       "not blocked by a sandbox), so a reused PID could not be ruled out; action not sent",
                       pid);
}

/// Whether @p err is the OS refusing for want of permission: an administrator may succeed (#2308).
[[nodiscard]] constexpr bool isPermissionError(int err) noexcept
{
    return err == EPERM || err == EACCES;
}

[[nodiscard]] std::string signalErrorMessage(int err)
{
    switch (err)
    {
    case EPERM:
        return "Permission denied - process belongs to another user";
    case ESRCH:
        return "Process not found - may have already exited";
    case EINVAL:
        return "Invalid signal";
    default:
        return std::system_category().message(err);
    }
}

/// Whether the process @p pidfd refers to has exited: 1 if it has, 0 if it is still running, -1 with
/// errno set if poll() fails. A pidfd polls readable once its process exits. Unlike signal 0, this
/// needs no permission over the process, so another user's live process (EPERM to a signal) is never
/// taken for an exited one (#803 review).
[[nodiscard]] int pidfdExited(const LinuxProcessSyscalls& syscalls, const FdGuard& pidfd)
{
    // A signal can interrupt even a zero-timeout poll(); that says nothing about the process, so it
    // is retried rather than reported as an unconfirmed identity (#803 review).
    int exited = syscalls.pollPidfd(pidfd.get());
    while (exited < 0 && errno == EINTR)
    {
        exited = syscalls.pollPidfd(pidfd.get());
    }
    if (exited < 0)
    {
        return -1;
    }
    return exited > 0 ? 1 : 0;
}

} // namespace

namespace LinuxProcessDetail
{

[[nodiscard]] PidfdOpen openPidfd(const LinuxProcessSyscalls& syscalls, std::int32_t pid)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - pid_t from <sys/types.h>, which clang-tidy 23 doesn't map here
    const int fd = syscalls.pidfdOpen(static_cast<pid_t>(pid));
    if (fd >= 0)
    {
        return {.fd = fd, .refusal = {}};
    }
    const int err = errno;
    // ENOSYS: a kernel before 5.3. EPERM/EACCES: pidfd_open never returns these itself, but a
    // seccomp filter does for syscalls it does not list (some container runtimes' default
    // profiles), so "process belongs to another user" would be the wrong message for them.
    if (err == ENOSYS || err == EPERM || err == EACCES)
    {
        return {.fd = -1, .refusal = noPidfdMessage(pid)};
    }
    return {.fd = -1, .refusal = signalErrorMessage(err)};
}

[[nodiscard]] ProcessActionResult verifyIdentity(const ProcessTarget& target)
{
    if (target.startTimeTicks == 0)
    {
        return checkProcessIdentity(target, 0);
    }
    const std::optional<std::uint64_t> actual = readStartTicks(target.pid);
    if (!actual.has_value())
    {
        return ProcessActionResult::error("Process not found - may have already exited");
    }
    return checkProcessIdentity(target, *actual);
}

[[nodiscard]] std::optional<std::string> readSmallFile(const std::string& path)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const FdGuard fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0)
    {
        return std::nullopt;
    }
    std::array<char, 4096> buf{};
    const auto len = ::read(fd.get(), buf.data(), buf.size());
    if (len < 0)
    {
        return std::nullopt;
    }
    return std::string(buf.data(), static_cast<std::size_t>(len));
}

} // namespace LinuxProcessDetail

namespace
{

using LinuxProcessDetail::openPidfd;
using LinuxProcessDetail::PidfdOpen;
using LinuxProcessDetail::readSmallFile;
using LinuxProcessDetail::sendThroughPidfd;
using LinuxProcessDetail::verifyIdentity;

/// Whether TaskSmack itself may set the Realtime I/O class: CAP_SYS_NICE or CAP_SYS_ADMIN in its own
/// effective set (ProcPrivileges::canSetRealtimeIoPriority()). Read once, by the constructor (#1540).
[[nodiscard]] bool ownProcessCanSetRealtimeIo()
{
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    return ProcPrivileges::canSetRealtimeIoPriority(status ? ProcPrivileges::parseCapEff(*status) : std::nullopt);
}

/// Whether thread `tid` is still in `pid`'s thread group. A TID belongs to one task at a time, so if
/// it is still listed under /proc/<pid>/task after setpriority(2), the call reached a thread of the
/// target: had the worker exited and its TID been reused by another process, it would be listed
/// under that process instead (#1228 review).
[[nodiscard]] bool realIsThreadOf(pid_t pid, id_t tid)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid) / "task" / std::to_string(tid), ec);
}

/// The thread IDs in /proc/<pid>/task, or why they couldn't be listed. There is no fallback to
/// the PID alone: renicing just the main thread and reporting success is the bug this replaces.
[[nodiscard]] std::expected<std::vector<id_t>, std::error_code> realListThreads(pid_t pid)
{
    std::vector<id_t> tids;
    std::error_code ec;
    std::filesystem::directory_iterator it(std::filesystem::path("/proc") / std::to_string(pid) / "task", ec);
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        id_t tid = 0;
        const auto [ptr, parseError] = std::from_chars(name.data(), name.data() + name.size(), tid);
        if (parseError == std::errc{} && ptr == name.data() + name.size() && tid > 0)
        {
            tids.push_back(tid);
        }
    }
    if (ec)
    {
        return std::unexpected(ec);
    }
    if (tids.empty())
    {
        return std::unexpected(std::make_error_code(std::errc::no_such_process));
    }
    return tids;
}

// The real system calls (realProcessSyscalls()). pidfd_open, pidfd_send_signal and the ioprio calls have
// no glibc wrappers, so they are raw syscalls; a kernel or libc without them says ENOSYS.
int realPidfdOpen(pid_t pid)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U));
#else
    static_cast<void>(pid);
    errno = ENOSYS;
    return -1;
#endif
}

int realPidfdSendSignal(int pidfd, int signal)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd, signal, nullptr, 0U));
#else
    static_cast<void>(pidfd);
    static_cast<void>(signal);
    errno = ENOSYS;
    return -1;
#endif
}

int realPollPidfd(int pidfd)
{
    pollfd entry{.fd = pidfd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&entry, 1, 0);
    if (ready < 0)
    {
        return -1;
    }
    return (ready > 0 && (entry.revents & (POLLIN | POLLHUP)) != 0) ? 1 : 0;
}

int realSetPriority(id_t tid, int nice)
{
    return ::setpriority(PRIO_PROCESS, tid, nice);
}

int realIoprioSet(id_t tid, int ioprio)
{
#ifdef SYS_ioprio_set
    return static_cast<int>(::syscall(SYS_ioprio_set, IoPrio::WHO_PROCESS, static_cast<pid_t>(tid), ioprio));
#else
    static_cast<void>(tid);
    static_cast<void>(ioprio);
    errno = ENOSYS;
    return -1;
#endif
}

int realSetAffinity(id_t tid, std::size_t setSize, const cpu_set_t* set)
{
    return ::sched_setaffinity(static_cast<pid_t>(tid), setSize, set);
}

pid_t realWaitPid(pid_t pid, int* status, int options)
{
    return ::waitpid(pid, status, options);
}

int realIoprioGet(pid_t pid)
{
#ifdef SYS_ioprio_get
    return static_cast<int>(::syscall(SYS_ioprio_get, IoPrio::WHO_PROCESS, pid));
#else
    static_cast<void>(pid);
    errno = ENOSYS;
    return -1;
#endif
}
} // namespace

LinuxProcessSyscalls realProcessSyscalls() noexcept
{
    return {.pidfdOpen = &realPidfdOpen,
            .pidfdSendSignal = &realPidfdSendSignal,
            .pollPidfd = &realPollPidfd,
            .setPriority = &realSetPriority,
            .ioprioSet = &realIoprioSet,
            .ioprioGet = &realIoprioGet,
            .setAffinity = &realSetAffinity,
            .listThreads = &realListThreads,
            .isThreadOf = &realIsThreadOf,
            .waitPid = &realWaitPid};
}

LinuxProcessControl::LinuxProcessControl() : LinuxProcessControl(realProcessSyscalls())
{}

LinuxProcessControl::LinuxProcessControl(const LinuxProcessSyscalls& syscalls)
    : m_Syscalls(syscalls), m_CanSetRealtimeIoPriority(ownProcessCanSetRealtimeIo())
{
    // A table that fakes only the kernel calls keeps the real thread listing (see LinuxProcessSyscalls).
    if (m_Syscalls.listThreads == nullptr)
    {
        m_Syscalls.listThreads = &realListThreads;
    }
    if (m_Syscalls.isThreadOf == nullptr)
    {
        m_Syscalls.isThreadOf = &realIsThreadOf;
    }
}

ProcessActionCapabilities LinuxProcessControl::actionCapabilities() const
{
    // No system call trace or dump files here: those start other programs, which LinuxProcessActions
    // adds (#2640).
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
        .canSendSignals = true, // HUP, INT, QUIT, USR1, USR2 (#3063)
        .canSetPriority = true,
        .canSetIoPriority = true, // ioprio_set(2) (#803)
        .canSetRealtimeIoPriority = m_CanSetRealtimeIoPriority,
        .canSetAffinity = m_Syscalls.setAffinity != nullptr, // sched_setaffinity(2) (#1887)
        .syscallTrace = SyscallTraceAvailability::Unsupported,
        .canCreateDump = false,
    };
}

ProcessActionResult LinuxProcessControl::terminate(const ProcessTarget& target)
{
    return sendSignalNumber(target, SIGTERM, "SIGTERM");
}

ProcessActionResult LinuxProcessControl::kill(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGKILL is provided by <signal.h>
    return sendSignalNumber(target, SIGKILL, "SIGKILL");
}

ProcessActionResult LinuxProcessControl::stop(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGSTOP is provided by <signal.h>
    return sendSignalNumber(target, SIGSTOP, "SIGSTOP");
}

ProcessActionResult LinuxProcessControl::resume(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGCONT is provided by <signal.h>
    return sendSignalNumber(target, SIGCONT, "SIGCONT");
}

ProcessActionResult LinuxProcessControl::sendSignal(const ProcessTarget& target, ProcessSignal signal)
{
    // NOLINTBEGIN(misc-include-cleaner) - the SIG* constants are provided by <signal.h>
    switch (signal)
    {
    case ProcessSignal::Hangup:
        return sendSignalNumber(target, SIGHUP, "SIGHUP");
    case ProcessSignal::Interrupt:
        return sendSignalNumber(target, SIGINT, "SIGINT");
    case ProcessSignal::Quit:
        return sendSignalNumber(target, SIGQUIT, "SIGQUIT");
    case ProcessSignal::User1:
        return sendSignalNumber(target, SIGUSR1, "SIGUSR1");
    case ProcessSignal::User2:
        return sendSignalNumber(target, SIGUSR2, "SIGUSR2");
    }
    // NOLINTEND(misc-include-cleaner)
    return ProcessActionResult::error("Unknown signal");
}

namespace
{

/// reniceThreads() against /proc and setpriority(2), through the syscall table.
[[nodiscard]] std::expected<PriorityChange, std::error_code>
setPriorityOfEveryThread(const LinuxProcessSyscalls& syscalls, int32_t pid, int32_t nice)
{
    return reniceThreads(
        pid,
        syscalls.listThreads,
        [&syscalls, nice](id_t tid) -> int
        {
            // setpriority() returns 0 on success and -1 on error (per POSIX).
            return syscalls.setPriority(tid, nice) == 0 ? 0 : errno;
        },
        syscalls.isThreadOf);
}

/// reniceThreads() against /proc and ioprio_set(2), through the syscall table. I/O priority is per thread on Linux, as
/// nice is: ioprio_set(IOPRIO_WHO_PROCESS, pid) alone changes only the main thread (#803, as #1104).
[[nodiscard]] std::expected<PriorityChange, std::error_code>
setIoPriorityOfEveryThread(const LinuxProcessSyscalls& syscalls, int32_t pid, int ioprio)
{
    return reniceThreads(
        pid,
        syscalls.listThreads,
        [&syscalls, ioprio](id_t tid) -> int { return syscalls.ioprioSet(tid, ioprio) == 0 ? 0 : errno; },
        syscalls.isThreadOf);
}

/// How the shared reporting below names the change: "priority" or "I/O priority".
struct ChangeWording
{
    std::string_view lower;   ///< Mid-sentence, e.g. "its priority was being changed".
    std::string_view leading; ///< At the start of a sentence, e.g. "Priority changed for".
};

constexpr ChangeWording NICE_WORDING{.lower = "priority", .leading = "Priority"};
constexpr ChangeWording IO_WORDING{.lower = "I/O priority", .leading = "I/O priority"};
constexpr ChangeWording AFFINITY_WORDING{.lower = "CPU affinity", .leading = "CPU affinity"};

/// The error for a failed first listing of the target's threads, for either change.
[[nodiscard]] ProcessActionResult threadListError(const ProcessTarget& target, std::error_code listError, ChangeWording wording)
{
    std::string errorMsg = (listError == std::errc::no_such_file_or_directory || listError == std::errc::no_such_process)
                             ? std::string("Process not found - may have already exited")
                             : std::format("Can't list the threads of process {}: {}", target.pid, listError.message());
    spdlog::warn("Failed to set {} for PID {}: {}", wording.lower, target.pid, errorMsg);
    return ProcessActionResult::error(std::move(errorMsg));
}

/// The outcome of a per-thread change (nice or I/O priority) to @p target, made through
/// reniceThreads() after the identity check; @p pidfd is the one opened before that check, and
/// @p firstErrorMessage the platform message for change.firstError, shown if any thread failed.
/// @p setting names the value set, for the success log: "nice=5", or "class=idle, level=0".
///
/// Once any thread was changed, it first confirms the target survived the change: if it exited
/// meanwhile, the change may have reached another process, which matters more than which threads
/// failed (#1228 review). Then exited workers, an incomplete relisting and threads that kept starting
/// are each reported, and only a change that reached every thread is a success.
[[nodiscard]] ProcessActionResult reportThreadChange(const LinuxProcessSyscalls& syscalls,
                                                     const PriorityChange& change,
                                                     const FdGuard& pidfd,
                                                     const ProcessTarget& target,
                                                     ChangeWording wording,
                                                     std::string_view setting,
                                                     std::string firstErrorMessage)
{
    if (change.changed > 0 || change.unconfirmed > 0)
    {
        // A pidfd polls readable once its process exits. Unlike signal 0, which needs kill rights
        // (same UID or CAP_KILL), this needs no permission over the process, so a change made to
        // another user's process with only CAP_SYS_NICE is not reported as unconfirmed (#1483).
        // A failing poll() leaves the change unconfirmed: it gives no evidence the target still
        // held the PID.
        const int exited = pidfdExited(syscalls, pidfd);
        if (exited != 0)
        {
            const int probeErr = errno;
            std::string errorMsg =
                (exited > 0)
                    ? std::format("Process {} exited while its {} was being changed; the change may have reached a different process",
                                  target.pid,
                                  wording.lower)
                    : std::format("{} was set, but it could not be confirmed that process {} still held its PID: {}",
                                  wording.leading,
                                  target.pid,
                                  std::system_category().message(probeErr));
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.unconfirmed > 0)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but {} thread(s) of process {} exited during the change; it may have reached "
                            "another process",
                            wording.leading,
                            change.changed,
                            change.unconfirmed,
                            target.pid);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.relistError)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but the threads of process {} couldn't be listed again ({}); any started since may "
                            "still have the old {}",
                            wording.leading,
                            change.changed,
                            target.pid,
                            change.relistError.message(),
                            wording.lower);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.threadsKeptStarting)
        {
            std::string errorMsg =
                std::format("{} changed for {} threads, but process {} kept starting new ones; some may still have the old {}",
                            wording.leading,
                            change.changed,
                            target.pid,
                            wording.lower);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.failed == 0)
        {
            spdlog::info("Successfully set {} ({}) for PID {} ({} threads)", wording.lower, setting, target.pid, change.changed);
            return ProcessActionResult::ok();
        }
    }

    std::string errorMsg = std::move(firstErrorMessage);
    if (change.changed > 0 || change.unconfirmed > 0)
    {
        errorMsg = std::format(
            "{} changed for only {} of {} threads. {}", wording.leading, change.changed, change.changed + change.failed, errorMsg);
    }

    spdlog::warn("Failed to set {} for PID {}: {}", wording.lower, target.pid, errorMsg);
    // A thread refused for want of permission (another user's process, or a raise without
    // CAP_SYS_NICE): the privileged helper may make the whole change (#2308).
    return isPermissionError(change.firstError) ? ProcessActionResult::denied(std::move(errorMsg))
                                                : ProcessActionResult::error(std::move(errorMsg));
}

} // namespace

ProcessActionResult LinuxProcessControl::setPriority(const ProcessTarget& target, int32_t nice)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }

    // Clamp nice value to valid range
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    spdlog::debug("Setting priority (nice={}) for PID {}", clampedNice, target.pid);

    // setpriority(2) has no pidfd form, so it can only be aimed at the PID. A pidfd still closes
    // the gap, after the fact: opened before the identity check as in sendSignalNumber(), it lets us
    // ask afterwards whether the target is still there. A PID is freed only when its process is
    // reaped, so if the target is still there after the call -- running or a zombie -- the PID
    // never changed hands and the call reached it.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set priority for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting priority for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // Nice is a per-thread attribute on Linux (setpriority(2), NOTES): PRIO_PROCESS with the PID
    // changes only the main thread, so a multithreaded compiler or browser kept nearly all of its
    // work at the old priority while the UI reported success (#1104). Renice every thread, as
    // Windows' SetPriorityClass changes the whole process.
    const auto result = setPriorityOfEveryThread(m_Syscalls, target.pid, clampedNice);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), NICE_WORDING);
    }
    return reportThreadChange(m_Syscalls,
                              *result,
                              pidfd,
                              target,
                              NICE_WORDING,
                              std::format("nice={}", clampedNice),
                              priorityErrorMessage(result->firstError, clampedNice, target.pid));
}

ProcessActionResult LinuxProcessControl::setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (static_cast<int>(ioClass) > IoPrio::MAX_CLASS)
    {
        return ProcessActionResult::error("Invalid I/O priority class");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }

    // encode() holds the level to 0-7, and to 0 for the classes without levels.
    const IoPriority priority{.ioClass = ioClass, .level = IoPrio::classHasLevels(ioClass) ? Domain::Priority::clampIoLevel(level) : 0};
    const int ioprio = IoPrio::encode(priority);
    const std::string setting = std::format("class={}, level={}", IoPrio::className(ioClass), priority.level);
    spdlog::debug("Setting I/O priority ({}) for PID {}", setting, target.pid);

    // As setPriority(): ioprio_set(2) has no pidfd form either, so the pidfd opened before the
    // identity check is asked afterwards whether the target survived the call.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set I/O priority for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting I/O priority for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // I/O priority is per thread, as nice is, so every thread is set (#803, as #1104).
    const auto result = setIoPriorityOfEveryThread(m_Syscalls, target.pid, ioprio);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), IO_WORDING);
    }
    return reportThreadChange(
        m_Syscalls, *result, pidfd, target, IO_WORDING, setting, ioPriorityErrorMessage(result->firstError, ioClass, target.pid));
}

namespace
{

/// A CPU_ALLOC()ed cpu_set_t, freed with CPU_FREE().
struct CpuSetDeleter
{
    void operator()(cpu_set_t* set) const noexcept
    {
        CPU_FREE(set);
    }
};

} // namespace

ProcessActionResult LinuxProcessControl::setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (cpus.empty())
    {
        return ProcessActionResult::error("Choose at least one CPU");
    }
    if (const std::optional<std::string> refused = schedulingChangeRefused(target.pid); refused.has_value())
    {
        return ProcessActionResult::error(*refused);
    }
    if (m_Syscalls.setAffinity == nullptr)
    {
        return ProcessActionResult::error("Setting CPU affinity isn't available on this system");
    }

    // A dynamically sized set (CPU_ALLOC), so a CPU at 1024 or above -- past the fixed cpu_set_t -- is
    // kept: the affinity read from Cpus_allowed_list has no width limit either (#1247).
    const std::size_t cpuCount = cpus.words().size() * CpuAffinity::BITS_PER_WORD;
    const std::unique_ptr<cpu_set_t, CpuSetDeleter> set(CPU_ALLOC(cpuCount));
    if (!set)
    {
        return ProcessActionResult::error("Out of memory");
    }
    const std::size_t setSize = CPU_ALLOC_SIZE(cpuCount);
    CPU_ZERO_S(setSize, set.get());
    for (std::size_t cpu = 0; cpu < cpuCount; ++cpu)
    {
        if (cpus.test(cpu))
        {
            CPU_SET_S(cpu, setSize, set.get());
        }
    }
    const std::string setting = std::format("{} CPUs", cpus.count());
    spdlog::debug("Setting CPU affinity ({}) for PID {}", setting, target.pid);

    // As setPriority(): sched_setaffinity(2) has no pidfd form, so the pidfd opened before the
    // identity check is asked afterwards whether the target survived the call.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to set CPU affinity for PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting CPU affinity for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // Affinity is per thread, as nice is: sched_setaffinity(2) with the PID changes only the main
    // thread, so every thread is set, threads started meanwhile included (#1887, as #1104).
    const cpu_set_t* const cpuSet = set.get();
    const auto result = reniceThreads(
        target.pid,
        m_Syscalls.listThreads,
        [this, setSize, cpuSet](id_t tid) -> int { return m_Syscalls.setAffinity(tid, setSize, cpuSet) == 0 ? 0 : errno; },
        m_Syscalls.isThreadOf);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), AFFINITY_WORDING);
    }
    return reportThreadChange(
        m_Syscalls, *result, pidfd, target, AFFINITY_WORDING, setting, affinityErrorMessage(result->firstError, target.pid));
}

CpuAffinityLayout LinuxProcessControl::affinityLayout() const
{
    // No processor groups on Linux. The online CPUs and, on a hybrid CPU, each one's class, read
    // when the Set affinity dialog opens (#1881), as the system probe reads them (#809).
    const std::filesystem::path cpuSysfsRoot("/sys/devices/system/cpu");
    CpuAffinityLayout layout;
    if (const auto online = LinuxCpuDetails::Detail::readFile(cpuSysfsRoot / "online"); online.has_value())
    {
        layout.online = CpuAffinity::fromCpuList(*online).value_or(CpuAffinity{});
    }
    layout.efficiencyClassByCpu = LinuxCpuDetails::readEfficiencyClasses(cpuSysfsRoot, std::nullopt);
    return layout;
}

IoPriorityReadResult LinuxProcessControl::getIoPriority(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return std::unexpected(std::string("Invalid PID"));
    }

    // Checked as an action is, so a reused PID's value is never shown for the target: the pidfd is
    // opened before the identity check and asked after the read whether the target still holds the
    // PID; if it does, the value read was the target's.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        return std::unexpected(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        return std::unexpected(std::move(identity.errorMessage));
    }

    // The main thread's value: the one ionice(1) shows, and the one setIoPriority() sets with the rest.
    const int raw = m_Syscalls.ioprioGet(static_cast<pid_t>(target.pid));
    if (raw < 0)
    {
        const int err = errno;
        if (err == ESRCH)
        {
            return std::unexpected(std::string("Process not found - may have already exited"));
        }
        return std::unexpected(
            std::format("Can't read the I/O priority of process {}: {}", target.pid, std::system_category().message(err)));
    }
    // Reading needs no privilege over the process, so neither may this check: another user's process
    // refuses signal 0 with EPERM while it is very much alive.
    const int exited = pidfdExited(m_Syscalls, pidfd);
    if (exited < 0)
    {
        return std::unexpected(std::format("Can't confirm that process {} still held its PID while its I/O priority was read: {}",
                                           target.pid,
                                           std::system_category().message(errno)));
    }
    if (exited > 0)
    {
        return std::unexpected(std::format("Process {} exited while its I/O priority was being read", target.pid));
    }

    const std::optional<IoPriority> decoded = IoPrio::decode(raw);
    if (!decoded.has_value())
    {
        return std::unexpected(std::format("Process {} has an I/O priority TaskSmack does not recognise ({})", target.pid, raw));
    }
    return *decoded;
}

ProcessActionResult LinuxProcessControl::sendSignalNumber(const ProcessTarget& target, int signal, std::string_view signalName) const
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    spdlog::debug("Sending {} to PID {}", signalName, target.pid);

    // The pidfd is opened *before* the start time is read. Whatever process it refers to held the
    // PID at that moment; if the start time read afterwards is the target's, that process is the
    // target, because the target is older than any process that could take the PID after it. From
    // then on the pidfd keeps referring to it, so a reuse of the PID cannot redirect the signal.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not sending {} to PID {}: {}", signalName, target.pid, identity.errorMessage);
        return identity;
    }
    if (const std::optional<std::string> nothing = signalWouldDoNothing(target.pid); nothing.has_value())
    {
        spdlog::info("Not sending {}: {}", signalName, *nothing);
        return ProcessActionResult::error(*nothing);
    }
    // Suspending itself would leave this process stopped with nothing to resume it: TaskSmack frozen, or
    // the root helper stopped for good, which the user can neither resume nor end (#2641).
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGSTOP is provided by <signal.h>
    if (signal == SIGSTOP && target.pid == ::getpid())
    {
        const std::string self = std::format("PID {} is this process, which would have nothing to resume it once suspended", target.pid);
        spdlog::info("Not sending {}: {}", signalName, self);
        return ProcessActionResult::error(self);
    }

    const int sendResult = sendThroughPidfd(m_Syscalls, pidfd, signal);

    if (sendResult == 0)
    {
        spdlog::info("Successfully sent {} to PID {}", signalName, target.pid);
        return ProcessActionResult::ok();
    }

    const int err = errno;
    const std::string errorMsg = signalErrorMessage(err);
    spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, errorMsg);
    // EPERM: another user's process, which the privileged helper may signal (#2308).
    return isPermissionError(err) ? ProcessActionResult::denied(errorMsg) : ProcessActionResult::error(errorMsg);
}

} // namespace Platform
