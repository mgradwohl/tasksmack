#include "LinuxProcessActions.h"

#include "Domain/PriorityConfig.h"
#include "Platform/IProcessActions.h"
#include "ProcStatStartTime.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

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

/// Result of trying to open a pidfd: the descriptor, or why there is none.
struct PidfdOpen
{
    int fd = -1;   ///< The pidfd, or -1.
    int error = 0; ///< errno of a real failure; 0 when the caller should fall back to the plain PID.
};

/// Open a pidfd for `pid`. Where pidfds are unavailable it reports no error, so the caller falls
/// back to acting on the PID straight after its identity check rather than failing the action.
[[nodiscard]] PidfdOpen openPidfd(std::int32_t pid)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int fd = static_cast<int>(::syscall(SYS_pidfd_open, static_cast<pid_t>(pid), 0U));
    if (fd >= 0)
    {
        return {.fd = fd, .error = 0};
    }
    const int err = errno;
    // ENOSYS: a kernel before 5.3. EPERM/EACCES: pidfd_open never returns these itself, but a
    // seccomp filter does for syscalls it does not list (some container runtimes' default
    // profiles); reporting that as "process belongs to another user" would be wrong, and kill(2)
    // can still carry the action out.
    if (err == ENOSYS || err == EPERM || err == EACCES)
    {
        return {.fd = -1, .error = 0};
    }
    return {.fd = -1, .error = err};
#else
    static_cast<void>(pid);
    return {.fd = -1, .error = 0};
#endif
}

/// Send `signal` through `pidfd` when there is one, else to the PID with kill(2).
[[nodiscard]] int signalTarget(const UniqueFd& pidfd, std::int32_t pid, int signal)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    if (pidfd.get() >= 0)
    {
        return static_cast<int>(::syscall(SYS_pidfd_send_signal, pidfd.get(), signal, nullptr, 0U));
    }
#else
    static_cast<void>(pidfd);
#endif
    return ::kill(pid, signal);
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
    if (opened.error != 0)
    {
        const std::string errorMsg = signalErrorMessage(opened.error);
        spdlog::warn("Failed to set priority for PID {}: {}", target.pid, errorMsg);
        return ProcessActionResult::error(errorMsg);
    }
    const UniqueFd pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not setting priority for PID {}: {}", target.pid, identity.errorMessage);
        return identity;
    }

    // setpriority() returns 0 on success and -1 on error (per POSIX).
    // Note: The errno-checking pattern applies to getpriority(), not setpriority().
    const int result = setpriority(PRIO_PROCESS, static_cast<id_t>(target.pid), clampedNice);
    if (result == 0)
    {
        // Signal 0 checks for existence without delivering anything. Without a pidfd (old kernel
        // or a sandbox) there is nothing to ask, and the gap is the few microseconds above.
        if (pidfd.get() >= 0 && signalTarget(pidfd, target.pid, 0) != 0 && errno == ESRCH)
        {
            std::string errorMsg = std::format(
                "Process {} exited while its priority was being changed; the change may have reached a different process", target.pid);
            spdlog::warn("{}", errorMsg);
            return ProcessActionResult::error(std::move(errorMsg));
        }
        spdlog::info("Successfully set priority (nice={}) for PID {}", clampedNice, target.pid);
        return ProcessActionResult::ok();
    }

    // Handle error
    const int err = errno;
    std::string errorMsg;

    switch (err)
    {
    case EPERM:
        errorMsg = "Permission denied. To lower priority (increase niceness), run TaskSmack as root or use: sudo renice -n " +
                   std::to_string(clampedNice) + " -p " + std::to_string(target.pid);
        break;
    case ESRCH:
        errorMsg = "Process not found - may have already exited";
        break;
    // Note: EACCES is not in POSIX for setpriority(), but is documented by
    // Linux setpriority(2) man page as a possible error code.
    case EACCES:
        errorMsg = "Permission denied. Try running TaskSmack with elevated privileges (sudo).";
        break;
    default:
        errorMsg = std::system_category().message(err);
        break;
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
    if (opened.error != 0)
    {
        const std::string errorMsg = signalErrorMessage(opened.error);
        spdlog::warn("Failed to send {} to PID {}: {}", signalName, target.pid, errorMsg);
        return ProcessActionResult::error(errorMsg);
    }
    const UniqueFd pidfd(opened.fd);

    ProcessActionResult identity = verifyIdentity(target);
    if (!identity.success)
    {
        spdlog::warn("Not sending {} to PID {}: {}", signalName, target.pid, identity.errorMessage);
        return identity;
    }

    // Without a pidfd (a kernel before 5.3, or a sandbox that blocks pidfd_open) the signal goes
    // to the PID with kill(2) straight after the check, which narrows the exposure to the gap
    // between the two calls but cannot close it.
    const int sendResult = signalTarget(pidfd, target.pid, signal);

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
