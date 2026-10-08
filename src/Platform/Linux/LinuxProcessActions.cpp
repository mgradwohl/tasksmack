#include "LinuxProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "IoPriority.h"
#include "Platform/IProcessActions.h"
#include "PosixGuards.h"
#include "PriorityErrorMessage.h"
#include "ProcParsing.h"
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
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides kill() function, csignal does not
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
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
[[nodiscard]] PidfdOpen openPidfd(std::int32_t pid)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0U));
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
#else
    return {.fd = -1, .refusal = noPidfdMessage(pid)};
#endif
}

/// Send `signal` through `pidfd`; 0 on success, -1 with errno set on failure.
[[nodiscard]] int sendThroughPidfd(const FdGuard& pidfd, int signal)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd.get(), signal, nullptr, 0U));
#else
    static_cast<void>(pidfd);
    static_cast<void>(signal);
    errno = ENOSYS;
    return -1;
#endif
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

} // namespace

ProcessActionCapabilities LinuxProcessActions::actionCapabilities() const
{
    return {
        .canTerminate = true,
        .canKill = true,
        .canStop = true,
        .canContinue = true,
        .canSetPriority = true,
        .canSetIoPriority = true, // ioprio_set(2) (#803)
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

/// Whether thread `tid` is still in `pid`'s thread group. A TID belongs to one task at a time, so if
/// it is still listed under /proc/<pid>/task after setpriority(2), the call reached a thread of the
/// target: had the worker exited and its TID been reused by another process, it would be listed
/// under that process instead (#1228 review).
[[nodiscard]] bool isThreadOf(int32_t pid, id_t tid)
{
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path("/proc") / std::to_string(pid) / "task" / std::to_string(tid), ec);
}

/// The thread IDs in /proc/<pid>/task, or why they couldn't be listed. There is no fallback to
/// the PID alone: renicing just the main thread and reporting success is the bug this replaces.
[[nodiscard]] std::expected<std::vector<id_t>, std::error_code> threadIds(int32_t pid)
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

/// reniceThreads() against the real /proc and setpriority(2).
[[nodiscard]] std::expected<PriorityChange, std::error_code> setPriorityOfEveryThread(int32_t pid, int32_t nice)
{
    return reniceThreads(
        pid,
        threadIds,
        [nice](id_t tid) -> int
        {
            // setpriority() returns 0 on success and -1 on error (per POSIX).
            return setpriority(PRIO_PROCESS, tid, nice) == 0 ? 0 : errno;
        },
        isThreadOf);
}

/// ioprio_set(2) for one thread: 0, or the errno. There is no glibc wrapper, so it is a raw syscall.
[[nodiscard]] int ioprioSet(id_t tid, int ioprio)
{
#ifdef SYS_ioprio_set
    return ::syscall(SYS_ioprio_set, IoPrio::WHO_PROCESS, static_cast<pid_t>(tid), ioprio) == 0 ? 0 : errno;
#else
    static_cast<void>(tid);
    static_cast<void>(ioprio);
    return ENOSYS;
#endif
}

/// ioprio_get(2) for one process: the ioprio value, or -1 with errno set.
[[nodiscard]] int ioprioGet(std::int32_t pid)
{
#ifdef SYS_ioprio_get
    return static_cast<int>(::syscall(SYS_ioprio_get, IoPrio::WHO_PROCESS, static_cast<pid_t>(pid)));
#else
    static_cast<void>(pid);
    errno = ENOSYS;
    return -1;
#endif
}

/// reniceThreads() against the real /proc and ioprio_set(2). I/O priority is per thread on Linux, as
/// nice is: ioprio_set(IOPRIO_WHO_PROCESS, pid) alone changes only the main thread (#803, as #1104).
[[nodiscard]] std::expected<PriorityChange, std::error_code> setIoPriorityOfEveryThread(int32_t pid, int ioprio)
{
    return reniceThreads(pid, threadIds, [ioprio](id_t tid) -> int { return ioprioSet(tid, ioprio); }, isThreadOf);
}

/// How the shared reporting below names the change: "priority" or "I/O priority".
struct ChangeWording
{
    std::string_view lower;   ///< Mid-sentence, e.g. "its priority was being changed".
    std::string_view leading; ///< At the start of a sentence, e.g. "Priority changed for".
};

constexpr ChangeWording NICE_WORDING{.lower = "priority", .leading = "Priority"};
constexpr ChangeWording IO_WORDING{.lower = "I/O priority", .leading = "I/O priority"};

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
[[nodiscard]] ProcessActionResult reportThreadChange(const PriorityChange& change,
                                                     const FdGuard& pidfd,
                                                     const ProcessTarget& target,
                                                     ChangeWording wording,
                                                     std::string_view setting,
                                                     std::string firstErrorMessage)
{
    if (change.changed > 0 || change.unconfirmed > 0)
    {
        // Signal 0 checks for existence without delivering anything. Any failure of that probe
        // leaves the call unconfirmed, not only ESRCH: a sandbox that blocks pidfd_send_signal
        // gives no evidence the target still held the PID.
        if (sendThroughPidfd(pidfd, 0) != 0)
        {
            const int probeErr = errno;
            std::string errorMsg =
                (probeErr == ESRCH)
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
    const PidfdOpen opened = openPidfd(target.pid);
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
    const auto result = setPriorityOfEveryThread(target.pid, clampedNice);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), NICE_WORDING);
    }
    return reportThreadChange(*result,
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
    const PidfdOpen opened = openPidfd(target.pid);
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
    const auto result = setIoPriorityOfEveryThread(target.pid, ioprio);
    if (!result.has_value())
    {
        return threadListError(target, result.error(), IO_WORDING);
    }
    return reportThreadChange(*result, pidfd, target, IO_WORDING, setting, ioPriorityErrorMessage(result->firstError, ioClass, target.pid));
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
    const PidfdOpen opened = openPidfd(target.pid);
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
    const int raw = ioprioGet(target.pid);
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
    if (sendThroughPidfd(pidfd, 0) != 0)
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

ProcessActionResult LinuxProcessActions::sendSignal(const ProcessTarget& target, int signal, std::string_view signalName)
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
    const PidfdOpen opened = openPidfd(target.pid);
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

    const int sendResult = sendThroughPidfd(pidfd, signal);

    if (sendResult == 0)
    {
        spdlog::info("Successfully sent {} to PID {}", signalName, target.pid);
        return ProcessActionResult::ok();
    }

    const std::string errorMsg = signalErrorMessage(errno);
    spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, errorMsg);
    return ProcessActionResult::error(errorMsg);
}

} // namespace Platform
