#include "LinuxProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "PriorityErrorMessage.h"
#include "ProcStatStartTime.h"

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
#include <unordered_set>
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

/// Owns a file descriptor, closing it on every exit path.
class UniqueFd
{
  public:
    explicit UniqueFd(int fd) noexcept : m_Fd(fd)
    {}
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&&) = delete;
    UniqueFd& operator=(UniqueFd&&) = delete;
    ~UniqueFd() noexcept
    {
        if (m_Fd >= 0)
        {
            ::close(m_Fd);
        }
    }

    [[nodiscard]] int get() const noexcept
    {
        return m_Fd;
    }

  private:
    int m_Fd = -1;
};

/// Start time of whatever process holds `pid` right now, or nullopt if there is none.
[[nodiscard]] std::optional<std::uint64_t> readStartTicks(std::int32_t pid)
{
    const std::string statPath = "/proc/" + std::to_string(pid) + "/stat";
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const UniqueFd fd(::open(statPath.c_str(), O_RDONLY | O_CLOEXEC));
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
    return ProcStat::parseStartTime(std::string_view(buf.data(), static_cast<std::size_t>(len)));
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
[[nodiscard]] int sendThroughPidfd(const UniqueFd& pidfd, int signal)
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

struct PriorityChange
{
    std::size_t changed = 0;
    std::size_t failed = 0;
    int firstError = 0;
    bool threadsKeptStarting = false; // new threads were still appearing after the last pass
};

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

/// setpriority(2) on every thread of `pid`. A thread can start another between the listing and its
/// own renice; the new one inherits the old nice and isn't in that listing. So the threads are
/// listed again after each pass until a pass finds none it hasn't already set, giving up after a
/// bounded number of passes (#1228 review). A thread that exits meanwhile (ESRCH) is neither a
/// change nor a failure -- except the main thread, whose exit means the process's. Only the first
/// listing's failure is an error; a later one means the process has gone, which the caller's
/// pidfd check reports.
[[nodiscard]] std::expected<PriorityChange, std::error_code> setPriorityOfEveryThread(int32_t pid, int32_t nice)
{
    constexpr int MAX_PASSES = 8;
    PriorityChange change;
    std::unordered_set<id_t> seen;
    for (int pass = 0; pass < MAX_PASSES; ++pass)
    {
        const auto tids = threadIds(pid);
        if (!tids.has_value())
        {
            if (pass == 0)
            {
                return std::unexpected(tids.error());
            }
            return change;
        }
        bool foundNew = false;
        for (const id_t tid : *tids)
        {
            if (!seen.insert(tid).second)
            {
                continue;
            }
            foundNew = true;
            // setpriority() returns 0 on success and -1 on error (per POSIX).
            if (setpriority(PRIO_PROCESS, tid, nice) == 0)
            {
                ++change.changed;
                continue;
            }
            const int err = errno;
            if (err == ESRCH && std::cmp_not_equal(tid, pid))
            {
                continue;
            }
            ++change.failed;
            if (change.firstError == 0)
            {
                change.firstError = err;
            }
        }
        if (!foundNew)
        {
            return change;
        }
    }
    change.threadsKeptStarting = true;
    return change;
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
    const UniqueFd pidfd(opened.fd);

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
        const std::error_code listError = result.error();
        std::string errorMsg = (listError == std::errc::no_such_file_or_directory || listError == std::errc::no_such_process)
                                 ? std::string("Process not found - may have already exited")
                                 : std::format("Can't list the threads of process {}: {}", target.pid, listError.message());
        spdlog::warn("Failed to set priority for PID {}: {}", target.pid, errorMsg);
        return ProcessActionResult::error(std::move(errorMsg));
    }
    const PriorityChange change = *result;

    // Once any thread was changed, confirm the target survived before reporting anything else: if
    // it exited meanwhile, the change may have reached another process, which matters more than
    // which threads failed (#1228 review).
    if (change.changed > 0)
    {
        // Signal 0 checks for existence without delivering anything. Any failure of that probe
        // leaves the call unconfirmed, not only ESRCH: a sandbox that blocks pidfd_send_signal
        // gives no evidence the target still held the PID.
        if (sendThroughPidfd(pidfd, 0) != 0)
        {
            const int probeErr = errno;
            std::string errorMsg =
                (probeErr == ESRCH)
                    ? std::format("Process {} exited while its priority was being changed; the change may have reached a different process",
                                  target.pid)
                    : std::format("Priority was set, but it could not be confirmed that process {} still held its PID: {}",
                                  target.pid,
                                  std::system_category().message(probeErr));
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.threadsKeptStarting)
        {
            std::string errorMsg =
                std::format("Priority changed for {} threads, but process {} kept starting new ones; some may still have the old priority",
                            change.changed,
                            target.pid);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        if (change.failed == 0)
        {
            spdlog::info("Successfully set priority (nice={}) for PID {} ({} threads)", clampedNice, target.pid, change.changed);
            return ProcessActionResult::ok();
        }
    }

    std::string errorMsg = priorityErrorMessage(change.firstError, clampedNice, target.pid);
    if (change.changed > 0)
    {
        errorMsg = std::format("Priority changed for only {} of {} threads. {}", change.changed, change.changed + change.failed, errorMsg);
    }

    spdlog::warn("Failed to set priority for PID {}: {}", target.pid, errorMsg);
    return ProcessActionResult::error(errorMsg);
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
    const UniqueFd pidfd(opened.fd);

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
