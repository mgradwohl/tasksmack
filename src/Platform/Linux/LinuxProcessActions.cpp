#include "LinuxProcessActions.h"

#include "DetachedSpawn.h"
#include "Domain/PriorityConfig.h"
#include "IoPriority.h"
#include "LinuxCoreDump.h"
#include "LinuxCpuDetails.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "PosixGuards.h"
#include "PriorityErrorMessage.h"
#include "ProcParsing.h"
#include "ProcPrivileges.h"
#include "SyscallTrace.h"
#include "ThreadPriority.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sched.h>

// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill() function, csignal does not
#include <signal.h>
#include <spawn.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace Platform
{

namespace
{

using Posix::FdGuard;

/// Start time of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<std::uint64_t> readStartTicks(std::int32_t pid)
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
    return ProcParsing::parseStatStartTime(std::string_view(buf.data(), static_cast<std::size_t>(len)));
}

/// Confirm the process holding the target's PID now is the one the target names.
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

/// Result of trying to open a pidfd: the descriptor, or the message to refuse the action with.
struct PidfdOpen
{
    int fd = -1;         ///< The pidfd, or -1.
    std::string refusal; ///< Why there is none; empty when fd is valid.
};

/// Error text shared by every action refused because pidfds are unavailable.
[[nodiscard]] std::string noPidfdMessage(std::int32_t pid)
{
    return std::format("Cannot act on process {} safely: this system does not provide pidfds (Linux 5.3 or later, "
                       "not blocked by a sandbox), so a reused PID could not be ruled out; action not sent",
                       pid);
}

[[nodiscard]] std::string signalErrorMessage(int err);

/// Open a pidfd for `pid`.
///
/// Without one there is no way to act on the checked process rather than on whatever holds its PID
/// a moment later, so the actions fail closed: they refuse rather than fall back to the bare PID,
/// as they refuse an unknown start time (#973). The supported Linux target (Ubuntu 24.04, kernel
/// 6.x) always has pidfd_open, so this only refuses on old kernels or under a seccomp filter.
[[nodiscard]] PidfdOpen openPidfd(const LinuxProcessSyscalls& syscalls, std::int32_t pid)
{
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

/// Send `signal` through `pidfd`; 0 on success, -1 with errno set on failure.
[[nodiscard]] int sendThroughPidfd(const LinuxProcessSyscalls& syscalls, const FdGuard& pidfd, int signal)
{
    return syscalls.pidfdSendSignal(pidfd.get(), signal);
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

/// Whether TaskSmack itself may set the Realtime I/O class: CAP_SYS_NICE or CAP_SYS_ADMIN in its own
/// effective set (ProcPrivileges::canSetRealtimeIoPriority()). Read once, by the constructors (#1540).
[[nodiscard]] bool ownProcessCanSetRealtimeIo();
} // namespace

namespace
{
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
            .isThreadOf = &realIsThreadOf};
}

namespace
{
// Defined with the other syscall-trace helpers below; the default for LinuxProcessSyscalls::ptraceContext.
[[nodiscard]] SyscallTrace::PtraceContext ptraceContextFor(std::int32_t pid, const std::string& tracerPath);
} // namespace

LinuxProcessActions::LinuxProcessActions() : LinuxProcessActions(discoverSyscallTraceTools(), realProcessSyscalls())
{}

LinuxProcessActions::LinuxProcessActions(SyscallTrace::Tools traceTools) : LinuxProcessActions(std::move(traceTools), realProcessSyscalls())
{}

LinuxProcessActions::LinuxProcessActions(SyscallTrace::Tools traceTools, const LinuxProcessSyscalls& syscalls)
    : m_TraceTools(std::move(traceTools)), m_Syscalls(syscalls), m_CanSetRealtimeIoPriority(ownProcessCanSetRealtimeIo())
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
    if (m_Syscalls.ptraceContext == nullptr)
    {
        m_Syscalls.ptraceContext = &ptraceContextFor;
    }
}

ProcessActionCapabilities LinuxProcessActions::actionCapabilities() const
{
    SyscallTraceAvailability syscallTrace = SyscallTraceAvailability::Available;
    if (m_TraceTools.tracerPath.empty())
    {
        syscallTrace = SyscallTraceAvailability::NoTracer;
    }
    else if (!m_TraceTools.terminal.has_value())
    {
        syscallTrace = SyscallTraceAvailability::NoTerminal;
    }
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
        .canSetPriority = true,
        .canSetIoPriority = true, // ioprio_set(2) (#803)
        .canSetRealtimeIoPriority = m_CanSetRealtimeIoPriority,
        .canSetAffinity = m_Syscalls.setAffinity != nullptr, // sched_setaffinity(2) (#1887)
        .syscallTrace = syscallTrace,
        .canCreateDump = !m_TraceTools.coreDumperPath.empty(), // gdb's gcore (#807)
    };
}

ProcessActionResult LinuxProcessActions::terminate(const ProcessTarget& target)
{
    return sendSignal(target, SIGTERM, "SIGTERM");
}

ProcessActionResult LinuxProcessActions::kill(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGKILL is provided by <signal.h>
    return sendSignal(target, SIGKILL, "SIGKILL");
}

ProcessActionResult LinuxProcessActions::stop(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGSTOP is provided by <signal.h>
    return sendSignal(target, SIGSTOP, "SIGSTOP");
}

ProcessActionResult LinuxProcessActions::resume(const ProcessTarget& target)
{
    // NOLINTNEXTLINE(misc-include-cleaner) - SIGCONT is provided by <signal.h>
    return sendSignal(target, SIGCONT, "SIGCONT");
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
    return ProcessActionResult::error(std::move(errorMsg));
}

} // namespace

ProcessActionResult LinuxProcessActions::setPriority(const ProcessTarget& target, int32_t nice)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }

    // Clamp nice value to valid range
    const int32_t clampedNice = Domain::Priority::clampNice(nice);

    spdlog::debug("Setting priority (nice={}) for PID {}", clampedNice, target.pid);

    // setpriority(2) has no pidfd form, so it can only be aimed at the PID. A pidfd still closes
    // the gap, after the fact: opened before the identity check as in sendSignal(), it lets us
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

ProcessActionResult LinuxProcessActions::setIoPriority(const ProcessTarget& target, IoPriorityClass ioClass, int32_t level)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (static_cast<int>(ioClass) > IoPrio::MAX_CLASS)
    {
        return ProcessActionResult::error("Invalid I/O priority class");
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

ProcessActionResult LinuxProcessActions::setAffinity(const ProcessTarget& target, const CpuAffinity& cpus)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    if (cpus.empty())
    {
        return ProcessActionResult::error("Choose at least one CPU");
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

CpuAffinityLayout LinuxProcessActions::affinityLayout() const
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

IoPriorityReadResult LinuxProcessActions::getIoPriority(const ProcessTarget& target)
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

namespace
{

/// Whether @p path is a regular file this process may execute.
[[nodiscard]] bool isExecutableFile(const std::string& path)
{
    struct stat info{};
    return ::stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(path.c_str(), X_OK) == 0;
}

/// The value of environment variable @p name, or empty when unset.
[[nodiscard]] std::string_view environmentValue(const char* name)
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once at construction, on the thread that builds the panels
    const char* const value = std::getenv(name);
    return value != nullptr ? std::string_view(value) : std::string_view{};
}

/// Up to 4 KiB of a small /proc or /sys file, or nullopt when it can't be read.
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

bool ownProcessCanSetRealtimeIo()
{
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    return ProcPrivileges::canSetRealtimeIoPriority(status ? ProcPrivileges::parseCapEff(*status) : std::nullopt);
}

/// Whether TaskSmack runs as root with CAP_SYS_PTRACE in its effective set, which strace exec'd from it
/// keeps (root keeps its effective capabilities across exec). An unreadable or malformed CapEff does
/// not count as having it (fail closed).
[[nodiscard]] bool rootWithSysPtrace()
{
    if (::geteuid() != 0)
    {
        return false;
    }
    const std::optional<std::string> status = readSmallFile("/proc/self/status");
    const std::optional<std::uint64_t> capEff = status ? ProcPrivileges::parseCapEff(*status) : std::nullopt;
    return SyscallTrace::rootTracerHasSysPtrace(true, capEff);
}

/// Whether the tool at @p tracerPath carries CAP_SYS_PTRACE as a file capability, whoever runs it.
[[nodiscard]] bool tracerHasFileCapability(const std::string& tracerPath)
{
    std::array<std::uint8_t, 64> xattr{};
    const auto len = ::getxattr(tracerPath.c_str(), "security.capability", xattr.data(), xattr.size());
    return len > 0 && SyscallTrace::fileCapsGrantSysPtrace(std::span<const std::uint8_t>(xattr.data(), static_cast<std::size_t>(len)));
}

/// What ptrace(2) will check when strace attaches to @p pid, read from /proc.
[[nodiscard]] SyscallTrace::PtraceContext ptraceContextFor(std::int32_t pid, const std::string& tracerPath)
{
    SyscallTrace::PtraceContext context;
    if (const std::optional<std::string> scope = readSmallFile("/proc/sys/kernel/yama/ptrace_scope"))
    {
        context.ptraceScope = SyscallTrace::parsePtraceScope(*scope);
    }
    const bool fileCapability = tracerHasFileCapability(tracerPath);
    const bool rootPrivilege = rootWithSysPtrace();
    context.privileged = fileCapability || rootPrivilege;
    context.privilegeIsInherited = rootPrivilege && !fileCapability;

    // ptrace requires the target's real, effective and saved IDs all to be the tracer's. A process
    // that is not dumpable (it changed credentials, or asked not to be) cannot be attached to either;
    // the kernel shows that by making root the owner of its /proc directory.
    const std::string procDir = "/proc/" + std::to_string(pid);
    const std::optional<std::string> status = readSmallFile(procDir + "/status");
    const auto uids = status ? SyscallTrace::parseStatusIds(*status, "Uid:") : std::nullopt;
    const auto gids = status ? SyscallTrace::parseStatusIds(*status, "Gid:") : std::nullopt;
    struct stat dirInfo{};
    const bool ownsProcDir = ::stat(procDir.c_str(), &dirInfo) == 0 && dirInfo.st_uid == ::geteuid();
    const auto allEqual = [](const std::optional<std::array<std::uint32_t, 3>>& ids, std::uint32_t expected)
    {
        return ids.has_value() && std::ranges::all_of(*ids, [expected](std::uint32_t id) { return id == expected; });
    };
    context.sameCredentials = ownsProcDir && allEqual(uids, ::geteuid()) && allEqual(gids, ::getegid());
    return context;
}

} // namespace

SyscallTrace::Tools LinuxProcessActions::discoverSyscallTraceTools()
{
    const std::string_view pathEnv = environmentValue("PATH");
    const auto find = [pathEnv](std::string_view name)
    {
        return SyscallTrace::findExecutable(name, pathEnv, isExecutableFile);
    };
    SyscallTrace::Tools tools;
    tools.tracerPath = find("strace").value_or(std::string{});
    tools.terminal = SyscallTrace::selectTerminal(environmentValue("TERMINAL"), find);
    tools.coreDumperPath = find("gcore").value_or(std::string{});
    spdlog::debug("System call tracing: strace {}, terminal {}; dump files: gcore {}",
                  tools.tracerPath.empty() ? std::string("not found") : tools.tracerPath,
                  tools.terminal ? tools.terminal->path : std::string("not found"),
                  tools.coreDumperPath.empty() ? std::string("not found") : tools.coreDumperPath);
    return tools;
}

ProcessActionResult LinuxProcessActions::launchSyscallTrace(const ProcessTarget& target)
{
    if (target.pid <= 0)
    {
        return ProcessActionResult::error("Invalid PID");
    }
    const SyscallTraceAvailability availability = actionCapabilities().syscallTrace;
    if (availability != SyscallTraceAvailability::Available || !m_TraceTools.terminal.has_value())
    {
        return ProcessActionResult::error(syscallTraceUnavailableReason(availability));
    }
    const SyscallTrace::Terminal& terminal = *m_TraceTools.terminal;

    // The same identity check as every other action: the pidfd is opened first, then the start time
    // compared, so the process checked is the one holding the PID now. strace then attaches by PID a
    // moment later -- it has no pidfd form -- so a target that exits in that instant and whose PID is
    // reused at once could still be the one traced; tracing only observes, and strace names the
    // process it attached to.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Not tracing PID {}: {}", target.pid, opened.refusal);
        return ProcessActionResult::error(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);
    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not tracing PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    const SyscallTrace::PtraceContext context = m_Syscalls.ptraceContext(target.pid, m_TraceTools.tracerPath);
    if (const std::optional<std::string> refusal = SyscallTrace::ptraceRefusal(target.pid, context))
    {
        spdlog::info("Not tracing PID {}: {}", target.pid, *refusal);
        return ProcessActionResult::error(*refusal);
    }
    // x-terminal-emulator and the like are links: what they resolve to decides who runs strace (#2140).
    std::error_code resolveError;
    const std::filesystem::path resolvedTerminal = std::filesystem::canonical(terminal.path, resolveError);
    if (const std::optional<std::string> refusal =
            SyscallTrace::serverTerminalRefusal(target.pid, context, resolveError ? terminal.path : resolvedTerminal.string()))
    {
        spdlog::info("Not tracing PID {}: {}", target.pid, *refusal);
        return ProcessActionResult::error(*refusal);
    }
    // Found at construction; make sure it has not been removed since, or the terminal would open and
    // close again at once.
    if (!isExecutableFile(m_TraceTools.tracerPath))
    {
        return ProcessActionResult::error(std::format("{} is no longer there or not executable", m_TraceTools.tracerPath));
    }

    const std::vector<std::string> command = SyscallTrace::tracerCommand(m_TraceTools.tracerPath, target.pid);
    const std::vector<std::string> argv = SyscallTrace::buildTerminalArgv(terminal, command);
    const auto spawned = DetachedSpawn::spawnDetached(argv);
    if (!spawned.has_value())
    {
        std::string message = DetachedSpawn::spawnFailureMessage(spawned.error(), terminal.path);
        spdlog::warn("Failed to open strace for PID {}: {}", target.pid, message);
        return ProcessActionResult::error(std::move(message));
    }
    spdlog::info("Opened {} running strace on PID {}", terminal.path, target.pid);
    return ProcessActionResult::ok();
}

ProcessActionResult LinuxProcessActions::sendSignal(const ProcessTarget& target, int signal, std::string_view signalName) const
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

    const int sendResult = sendThroughPidfd(m_Syscalls, pidfd, signal);

    if (sendResult == 0)
    {
        spdlog::info("Successfully sent {} to PID {}", signalName, target.pid);
        return ProcessActionResult::ok();
    }

    const std::string errorMsg = signalErrorMessage(errno);
    spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, errorMsg);
    return ProcessActionResult::error(errorMsg);
}

namespace
{

/// The name the kernel gives @p pid (/proc/<pid>/comm) without its newline; empty when it can't be read.
[[nodiscard]] std::string processComm(std::int32_t pid)
{
    std::string comm = readSmallFile(std::format("/proc/{}/comm", pid)).value_or(std::string{});
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == '\r'))
    {
        comm.pop_back();
    }
    return comm;
}

/// Whether @p pid is stopped, by a signal or a tracer ('T' or 't' in /proc/<pid>/stat), so a cancelled
/// dump knows whether to wake it.
[[nodiscard]] bool processIsStopped(std::int32_t pid)
{
    const std::string stat = readSmallFile(std::format("/proc/{}/stat", pid)).value_or(std::string{});
    return LinuxCoreDump::isStoppedState(LinuxCoreDump::stateFromStat(stat).value_or('\0'));
}

/// Who traces @p pid (its TracerPid; 0 for nobody), or nullopt once its status can't be read (it is gone).
[[nodiscard]] std::optional<std::int32_t> processTracer(std::int32_t pid)
{
    const std::optional<std::string> status = readSmallFile(std::format("/proc/{}/status", pid));
    return status.has_value() ? LinuxCoreDump::tracerPidFromStatus(*status) : std::nullopt;
}

/// Waits, up to LinuxCoreDump::DETACH_WAIT_MS, until @p pid is traced by @p tracerBefore again (whoever
/// traced it before the dump; usually nobody): the killed gdb has exited and let go of it (#2122).
void waitForDumperToDetach(std::int32_t pid, std::int32_t tracerBefore)
{
    constexpr auto STEP = std::chrono::milliseconds(10);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(LinuxCoreDump::DETACH_WAIT_MS);
    for (;;)
    {
        const std::optional<std::int32_t> tracer = processTracer(pid);
        if (!tracer.has_value() || *tracer == tracerBefore || std::chrono::steady_clock::now() >= deadline)
        {
            return;
        }
        std::this_thread::sleep_for(STEP);
    }
}

/// How a tool run by runCancellable() ended.
struct ToolRun
{
    std::error_code spawnError; ///< Set when it couldn't be started.
    bool cancelled = false;     ///< Killed because the stop token was requested.
    int exitStatus = -1;        ///< Its exit status when it exited on its own; -1 when a signal ended it.
    std::string stderrText;     ///< The last of what it wrote on stderr (MAX_STDERR_KEPT bytes at most).
};

constexpr std::size_t MAX_STDERR_KEPT = 4096;
constexpr int TOOL_POLL_MS = 50;

/// Appends what @p fd has ready to @p text, keeping its last MAX_STDERR_KEPT bytes. False at end of file.
bool readAvailable(int fd, std::string& text)
{
    std::array<char, 1024> chunk{};
    const ssize_t got = ::read(fd, chunk.data(), chunk.size()); // NOLINT(misc-include-cleaner) - ssize_t is from <sys/types.h>
    if (got <= 0)
    {
        return got < 0 && errno == EINTR;
    }
    text.append(chunk.data(), static_cast<std::size_t>(got));
    if (text.size() > MAX_STDERR_KEPT)
    {
        text.erase(0, text.size() - MAX_STDERR_KEPT);
    }
    return true;
}

/// Runs @p argv in a process group of its own, stdin and stdout on /dev/null and stderr kept, until it
/// exits or @p stopToken is requested; then the whole group is killed (gcore runs gdb under it) and
/// reaped. Blocking: called on createDump()'s worker thread.
[[nodiscard]] ToolRun runCancellable(const std::vector<std::string>& argv, const std::stop_token& stopToken)
{
    ToolRun run;
    std::array<int, 2> pipeFds{-1, -1};
    if (::pipe2(pipeFds.data(), O_CLOEXEC) != 0)
    {
        run.spawnError = std::error_code(errno, std::generic_category());
        return run;
    }
    const FdGuard readEnd(pipeFds[0]);
    FdGuard writeEnd(pipeFds[1]);

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_init(&attributes);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), STDERR_FILENO);
    // Its own process group, so a cancel reaches the gdb gcore runs; an empty signal mask and default
    // dispositions, whatever this thread blocks or ignores.
    sigset_t signals{}; // NOLINT(misc-include-cleaner) - sigset_t is provided by <signal.h>
    sigemptyset(&signals);
    ::posix_spawnattr_setsigmask(&attributes, &signals);
    sigaddset(&signals, SIGPIPE);
    ::posix_spawnattr_setsigdefault(&attributes, &signals);
    ::posix_spawnattr_setpgroup(&attributes, 0);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
    {
        args.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast) - posix_spawn's char* const[]
    }
    args.push_back(nullptr);
    pid_t child = -1;
    const int spawned = ::posix_spawn(&child, argv.front().c_str(), &actions, &attributes, args.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    {
        const FdGuard closing(std::move(writeEnd)); // Only the child writes: end of file once it is gone.
    }
    if (spawned != 0)
    {
        run.spawnError = std::error_code(spawned, std::generic_category());
        return run;
    }

    for (;;)
    {
        pollfd ready{.fd = readEnd.get(), .events = POLLIN, .revents = 0};
        if (::poll(&ready, 1, TOOL_POLL_MS) > 0 && (ready.revents & (POLLIN | POLLHUP)) != 0)
        {
            static_cast<void>(readAvailable(readEnd.get(), run.stderrText));
        }
        int status = 0;
        // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child)
        {
            // Whatever it wrote last, up to the end of the pipe (or until nothing more is ready).
            pollfd rest{.fd = readEnd.get(), .events = POLLIN, .revents = 0};
            while (::poll(&rest, 1, 0) > 0 && readAvailable(readEnd.get(), run.stderrText))
            {
            }
            // NOLINTNEXTLINE(misc-include-cleaner) - WIFEXITED/WEXITSTATUS are provided by <sys/wait.h>
            run.exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            return run;
        }
        if (waited < 0 && errno != EINTR)
        {
            return run;
        }
        if (stopToken.stop_requested())
        {
            ::kill(-child, SIGKILL);
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
            {
            }
            run.cancelled = true;
            return run;
        }
    }
}

} // namespace

ProcessDumpResult
LinuxProcessActions::createDump(const ProcessTarget& target, const std::filesystem::path& directory, const std::stop_token& stopToken)
{
    const std::string& gcore = m_TraceTools.coreDumperPath;
    if (gcore.empty())
    {
        return std::unexpected(std::string(DUMP_UNSUPPORTED_MESSAGE));
    }
    if (target.pid <= 0)
    {
        return std::unexpected(std::string("Invalid PID"));
    }
    // The identity check every action makes. gcore then attaches by PID -- it has no pidfd form -- so, as
    // with strace, a target that exits in that instant and whose PID is reused at once could be the one
    // dumped; the core names the process it was taken from.
    const PidfdOpen opened = openPidfd(m_Syscalls, target.pid);
    if (opened.fd < 0)
    {
        spdlog::warn("Not dumping PID {}: {}", target.pid, opened.refusal);
        return std::unexpected(opened.refusal);
    }
    const FdGuard pidfd(opened.fd);
    if (const ProcessActionResult identity = verifyIdentity(target); !identity.success)
    {
        spdlog::warn("Not dumping PID {}: {}", target.pid, identity.errorMessage);
        return std::unexpected(identity.errorMessage);
    }
    if (const std::optional<std::string> refusal =
            SyscallTrace::ptraceRefusal(target.pid, m_Syscalls.ptraceContext(target.pid, gcore), SyscallTrace::GCORE_TOOL))
    {
        spdlog::info("Not dumping PID {}: {}", target.pid, *refusal);
        return std::unexpected(*refusal);
    }
    if (!isExecutableFile(gcore))
    {
        return std::unexpected(std::format("{} is no longer there or not executable", gcore));
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec))
    {
        return std::unexpected(std::format("The folder {} doesn't exist", directory.string()));
    }

    // A name not taken yet: the process name, PID and local time, then " (2)" and on.
    const std::string comm = processComm(target.pid);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    std::filesystem::path file;
    for (int attempt = 1; attempt <= LinuxCoreDump::MAX_NAME_ATTEMPTS && file.empty(); ++attempt)
    {
        std::filesystem::path candidate = directory / LinuxCoreDump::dumpFileName(comm, target.pid, local, attempt);
        if (!std::filesystem::exists(candidate, ec))
        {
            file = std::move(candidate);
        }
    }
    if (file.empty())
    {
        return std::unexpected(std::format("Couldn't find a free name for the dump file in {}", directory.string()));
    }

    // gcore writes "<prefix>.<pid>"; the prefix marks it partial until it is renamed into place.
    const std::filesystem::path prefix = file.string() + ".partial";
    const std::filesystem::path written = LinuxCoreDump::gcoreOutputFile(prefix, target.pid);
    const bool wasStopped = processIsStopped(target.pid);
    const std::int32_t tracerBefore = processTracer(target.pid).value_or(0);
    spdlog::info("Writing a dump of PID {} with {} to {}", target.pid, gcore, file.string());
    const ToolRun run = runCancellable(LinuxCoreDump::gcoreArgv(gcore, prefix.string(), target.pid), stopToken);
    if (run.spawnError)
    {
        return std::unexpected(std::format("Couldn't start {}: {}", gcore, run.spawnError.message()));
    }
    if (run.cancelled)
    {
        std::filesystem::remove(written, ec);
        // gdb was killed while attached: the kernel detaches it, but a process its attach stopped may
        // stay stopped. Killing gcore's group doesn't wait for gdb (gcore's child) to exit, and until it
        // has, the process shows 't' and its stop is still to come (#2122): wait for gdb to let go first.
        // One that was running before is then woken, through the pidfd, so it is still the target.
        waitForDumperToDetach(target.pid, tracerBefore);
        if (!wasStopped && processIsStopped(target.pid))
        {
            static_cast<void>(sendThroughPidfd(m_Syscalls, pidfd, SIGCONT));
        }
        spdlog::info("Dump of PID {} cancelled", target.pid);
        return std::unexpected(std::string(LinuxCoreDump::CANCELLED_MESSAGE));
    }
    if (run.exitStatus != 0 || !std::filesystem::exists(written, ec))
    {
        std::filesystem::remove(written, ec);
        std::string message = LinuxCoreDump::gcoreFailureMessage(target.pid, run.exitStatus, run.stderrText);
        spdlog::warn("Dump of PID {} failed: {}", target.pid, message);
        return std::unexpected(std::move(message));
    }
    std::filesystem::rename(written, file, ec);
    if (ec)
    {
        std::filesystem::remove(written, ec);
        return std::unexpected(std::format("Couldn't name the dump file {}: {}", file.string(), ec.message()));
    }
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    spdlog::info("Wrote a dump of PID {} to {}", target.pid, file.string());
    return ProcessDump{.file = file,
                       .sizeBytes = ec ? 0 : static_cast<std::uint64_t>(size),
                       .note = {},
                       .fromSnapshot = false,
                       .liveReason = LinuxCoreDump::LIVE_REASON};
}

DumpSpace LinuxProcessActions::dumpSpace(const ProcessTarget& target, const std::filesystem::path& directory)
{
    DumpSpace space;
    struct statvfs stats{};
    if (!directory.empty() && ::statvfs(directory.c_str(), &stats) == 0)
    {
        space.freeBytes = static_cast<std::uint64_t>(stats.f_bavail) * static_cast<std::uint64_t>(stats.f_frsize);
    }
    if (target.pid > 0)
    {
        // Read first, then confirmed to have been the target's: a PID reused in between is not counted.
        std::ifstream file(std::format("/proc/{}/status", target.pid));
        const std::string status{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (!status.empty() && verifyIdentity(target).success)
        {
            space.processBytes = LinuxCoreDump::coreBytesFromStatus(status);
        }
    }
    return space;
}

} // namespace Platform
