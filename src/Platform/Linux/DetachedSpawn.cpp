#include "DetachedSpawn.h"

#include "PosixGuards.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t, rlimit, wait macros
#include <fcntl.h>
// NOLINTNEXTLINE(modernize-deprecated-headers) - POSIX signal.h provides sigaction(), csignal does not
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// NOLINTEND(misc-include-cleaner)

namespace Platform::DetachedSpawn
{

namespace
{

using Posix::FdGuard;

/// How long to wait for the detached child to report that execve() failed (or, by closing the pipe,
/// that it succeeded). An exec takes milliseconds; the cap only bounds a pathological stall (a hung
/// network filesystem) so the UI thread is never held for long.
constexpr int EXEC_REPORT_TIMEOUT_MS = 2000;

/// Highest descriptor (exclusive) the last-resort fallback walks when RLIMIT_NOFILE is unlimited:
/// the kernel's default fs.nr_open, which no descriptor can exceed unless an administrator raised it.
constexpr int UNLIMITED_FD_LIMIT = 1 << 20;

/// What a child writes to the report pipe when it fails: the stage and errno.
struct Report
{
    std::int32_t stage = 0;
    std::int32_t error = 0;
};

/// Write @p report to @p fd. Async-signal-safe: only write(2).
void writeReport(int fd, SpawnFailure::Stage stage, int error) noexcept
{
    const Report report{.stage = static_cast<std::int32_t>(stage), .error = error};
    // Smaller than PIPE_BUF, so one write is atomic; nothing useful can be done if it fails.
    [[maybe_unused]] const auto written = ::write(fd, &report, sizeof(report));
}

/// Mark @p fd close-on-exec. Async-signal-safe: fcntl(2) only. A descriptor closed since the
/// snapshot fails with EBADF, which is harmless.
void setCloseOnExec(int fd) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX fcntl() is variadic by definition
    static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
}

/// Everything the detached child needs, prepared before fork() so the child never allocates.
struct ChildPlan
{
    const char* path = nullptr;
    char* const* argv = nullptr;
    char* const* envp = nullptr;
    int devNull = -1;  ///< Above STDERR_FILENO.
    int reportFd = -1; ///< Write end of the report pipe; above STDERR_FILENO.
    bool useCloseRange = true;
    std::span<const int> openFds; ///< Descriptors open before fork(), from /proc/self/fd.
    bool openFdsKnown = false;    ///< Whether openFds could be listed.
    int fdLimit = 0;              ///< Last resort when they couldn't: walk 3 .. fdLimit-1.
    const struct sigaction* defaultAction = nullptr;
    // NOLINTNEXTLINE(misc-include-cleaner) - sigset_t is provided by <signal.h>
    const sigset_t* emptyMask = nullptr;
};

/// Mark every descriptor from 3 up close-on-exec, keeping them open until execve() so the report pipe
/// still works if it fails. Async-signal-safe: syscall(2) and fcntl(2) only.
void markInheritedFdsCloseOnExec(const ChildPlan& plan) noexcept
{
#if defined(SYS_close_range) && defined(CLOSE_RANGE_CLOEXEC)
    if (plan.useCloseRange && ::syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) == 0)
    {
        return;
    }
#endif
    if (plan.openFdsKnown)
    {
        // Every descriptor open when spawnDetached() began. One another thread opens after that
        // snapshot without O_CLOEXEC could still leak here; TaskSmack opens its own with O_CLOEXEC.
        for (const int fd : plan.openFds)
        {
            if (fd > STDERR_FILENO)
            {
                setCloseOnExec(fd);
            }
        }
        return;
    }
    for (int fd = STDERR_FILENO + 1; fd < plan.fdLimit; ++fd)
    {
        setCloseOnExec(fd);
    }
}

/// The detached program's process: reset what it would otherwise inherit from TaskSmack, then exec.
/// Never returns. Async-signal-safe calls only (signal(7)).
[[noreturn]] void execDetached(const ChildPlan& plan) noexcept
{
    // Default dispositions: exec keeps ignored signals ignored (SIGPIPE, for one), which the terminal
    // and strace would then inherit. sigaction() refuses SIGKILL, SIGSTOP and libc's reserved signals;
    // that is harmless.
    for (int sig = 1; sig < NSIG; ++sig)
    {
        static_cast<void>(::sigaction(sig, plan.defaultAction, nullptr));
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - this forked child is single-threaded, and sigprocmask is async-signal-safe
    static_cast<void>(::sigprocmask(SIG_SETMASK, plan.emptyMask, nullptr));

    // stdin/stdout/stderr on /dev/null: the terminal draws its own window and must not write into
    // whatever TaskSmack was started from. dup2() clears close-on-exec on the copies. /dev/null and the
    // report pipe are above STDERR_FILENO, so these never overwrite the pipe.
    if (::dup2(plan.devNull, STDIN_FILENO) < 0 || ::dup2(plan.devNull, STDOUT_FILENO) < 0 || ::dup2(plan.devNull, STDERR_FILENO) < 0)
    {
        writeReport(plan.reportFd, SpawnFailure::Stage::Exec, errno);
        ::_exit(127);
    }
    markInheritedFdsCloseOnExec(plan);

    ::execve(plan.path, plan.argv, plan.envp);
    writeReport(plan.reportFd, SpawnFailure::Stage::Exec, errno);
    ::_exit(127);
}

/// @p fd, moved above STDERR_FILENO (close-on-exec) if it is one of 0-2, which happens when TaskSmack
/// was started with its standard descriptors closed. The low descriptor is closed again, as it was.
/// Returns -1 with errno set if it could not be moved.
[[nodiscard]] int moveAboveStdio(int fd) noexcept
{
    if (fd < 0 || fd > STDERR_FILENO)
    {
        return fd;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX fcntl() is variadic by definition
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    const int err = errno;
    ::close(fd);
    errno = err;
    return moved;
}

/// The descriptors open in this process now, from /proc/self/fd (including the one used to list
/// it, which is closed again by the time the list is used; harmless). Empty when it can't be read.
[[nodiscard]] std::vector<int> listOpenFds(bool& known)
{
    std::vector<int> fds;
    std::error_code ec;
    std::filesystem::directory_iterator it("/proc/self/fd", ec);
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec))
    {
        const std::string name = it->path().filename().string();
        int fd = -1;
        const auto [ptr, parseError] = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (parseError == std::errc{} && ptr == name.data() + name.size())
        {
            fds.push_back(fd);
        }
    }
    known = !ec;
    return fds;
}

/// The last-resort fallback's bound: the whole finite RLIMIT_NOFILE (no descriptor can be at or above
/// it), else the kernel's default ceiling.
[[nodiscard]] int descriptorLimit() noexcept
{
    rlimit fileLimit{};
    if (::getrlimit(RLIMIT_NOFILE, &fileLimit) == 0 && fileLimit.rlim_cur != RLIM_INFINITY)
    {
        return fileLimit.rlim_cur > static_cast<rlim_t>(INT_MAX) ? INT_MAX : static_cast<int>(fileLimit.rlim_cur);
    }
    return UNLIMITED_FD_LIMIT;
}

} // namespace

std::string spawnFailureMessage(const SpawnFailure& failure, const std::string& program)
{
    const std::string reason = std::system_category().message(failure.error);
    switch (failure.stage)
    {
    case SpawnFailure::Stage::Exec:
        if (failure.error == ENOENT)
        {
            return std::format("Could not start {}: it no longer exists", program);
        }
        if (failure.error == EACCES)
        {
            return std::format("Could not start {}: permission denied", program);
        }
        return std::format("Could not start {}: {}", program, reason);
    case SpawnFailure::Stage::Fork:
        return std::format("Could not start a process for {}: {}", program, reason);
    case SpawnFailure::Stage::Setup:
        break;
    }
    return std::format("Could not prepare to start {}: {}", program, reason);
}

std::expected<void, SpawnFailure> spawnDetached(std::span<const std::string> argv)
{
    return Detail::spawnDetached(argv, Detail::SpawnHooks{});
}

std::expected<void, SpawnFailure> Detail::spawnDetached(std::span<const std::string> argv, const SpawnHooks& hooks)
{
    if (argv.empty() || !argv.front().starts_with('/'))
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = EINVAL});
    }

    // Everything the children need is built here, before fork(): after it they may not allocate.
    std::vector<char*> argPointers;
    argPointers.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
    {
        // execve() takes char* const[] but does not modify the strings.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) - execve's argv type predates const
        argPointers.push_back(const_cast<char*>(arg.c_str()));
    }
    argPointers.push_back(nullptr);

    // Opened first, then each moved above STDERR_FILENO: with 0-2 closed, /dev/null would be 0 and
    // the pipe 1 and 2, and the child's dup2() onto 0-2 would replace the report pipe with /dev/null.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const int devNullRaw = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devNullRaw < 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = errno});
    }
    std::array<int, 2> reportPipe{-1, -1};
    if (::pipe2(reportPipe.data(), O_CLOEXEC) != 0)
    {
        const int err = errno;
        ::close(devNullRaw);
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = err});
    }
    // Each moved only after all three are open, so a closed 0-2 is not reused by the next one.
    const FdGuard devNull(moveAboveStdio(devNullRaw));
    const FdGuard reportRead(moveAboveStdio(reportPipe[0]));
    FdGuard reportWrite(moveAboveStdio(reportPipe[1]));
    if (devNull.get() < 0 || reportRead.get() < 0 || reportWrite.get() < 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = errno});
    }

    // Listed even when close_range() is expected to work: it can still fail in the child (a kernel
    // before 5.11, or a seccomp filter), and the child cannot list /proc itself without allocating.
    bool openFdsKnown = false;
    const std::vector<int> openFds = listOpenFds(openFdsKnown);
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    sigset_t emptyMask{};
    sigemptyset(&emptyMask);

    const ChildPlan plan{
        .path = argPointers.front(),
        .argv = argPointers.data(),
        .envp = environ,
        .devNull = devNull.get(),
        .reportFd = reportWrite.get(),
        .useCloseRange = hooks.useCloseRange,
        .openFds = openFds,
        .openFdsKnown = openFdsKnown,
        .fdLimit = descriptorLimit(),
        .defaultAction = &defaultAction,
        .emptyMask = &emptyMask,
    };

    const pid_t intermediate = ::fork();
    if (intermediate < 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Fork, .error = errno});
    }
    if (intermediate == 0)
    {
        // TaskSmack's child: a session of its own, so the terminal is not in TaskSmack's process group
        // and session (a Ctrl+C or hangup aimed at those does not reach it); then fork the program's
        // process and exit, orphaning it to init.
        static_cast<void>(::setsid());
        const pid_t program = ::fork();
        if (program < 0)
        {
            writeReport(plan.reportFd, SpawnFailure::Stage::Fork, errno);
            ::_exit(1);
        }
        if (program == 0)
        {
            execDetached(plan);
        }
        ::_exit(0);
    }

    // Only the children write to the pipe: with our copy closed, end-of-file means both are done with
    // it -- the intermediate exited and the program exec'd (close-on-exec) -- without a failure report.
    reportWrite = FdGuard(-1);

    // Reap the intermediate child, which exits as soon as it has forked: this is the only wait, and it
    // is not on the program.
    int status = 0;
    pid_t waited = ::waitpid(intermediate, &status, 0);
    while (waited < 0 && errno == EINTR)
    {
        waited = ::waitpid(intermediate, &status, 0);
    }
    if (waited < 0 && errno != ECHILD) // ECHILD: SIGCHLD is ignored, so the kernel reaped it already
    {
        spdlog::warn(
            "DetachedSpawn: waitpid for the intermediate child of {} failed: {}", argv.front(), std::system_category().message(errno));
    }

    // The bound on this wait is the poll: read() below runs only once the pipe is readable (a report,
    // or end of file), so it never blocks.
    const auto pollFn = hooks.poll != nullptr ? hooks.poll : &::poll;
    pollfd pending{.fd = reportRead.get(), .events = POLLIN, .revents = 0};
    int ready = pollFn(&pending, 1, EXEC_REPORT_TIMEOUT_MS);
    while (ready < 0 && errno == EINTR)
    {
        ready = pollFn(&pending, 1, EXEC_REPORT_TIMEOUT_MS);
    }
    if (ready < 0)
    {
        // As for a timeout: the program may still exec, so it is not reported as failed (the user
        // would start a second one), and a blocking read() here could hold the UI thread indefinitely.
        spdlog::warn("DetachedSpawn: poll for the exec report of {} failed ({}); assuming it started",
                     argv.front(),
                     std::system_category().message(errno));
        return {};
    }
    if (ready == 0)
    {
        spdlog::warn("DetachedSpawn: {} has not reported its exec after {} ms; assuming it started", argv.front(), EXEC_REPORT_TIMEOUT_MS);
        return {};
    }
    Report report{};
    auto got = ::read(reportRead.get(), &report, sizeof(report));
    while (got < 0 && errno == EINTR)
    {
        got = ::read(reportRead.get(), &report, sizeof(report));
    }
    if (std::cmp_equal(got, sizeof(report)))
    {
        return std::unexpected(SpawnFailure{.stage = static_cast<SpawnFailure::Stage>(report.stage), .error = report.error});
    }
    // End of file: exec succeeded. A short read cannot happen (the report is written whole).
    return {};
}

} // namespace Platform::DetachedSpawn
