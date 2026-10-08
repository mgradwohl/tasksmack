#include "DetachedSpawn.h"

#include "PosixGuards.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
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

/// Highest descriptor (exclusive) the pre-5.11 fallback marks close-on-exec one by one. Bounded so a
/// huge RLIMIT_NOFILE (systemd raises it to 2^20 or more) cannot make that loop slow.
constexpr int FALLBACK_FD_LIMIT = 65536;

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

/// Mark every descriptor from 3 up close-on-exec, keeping them open until execve() so the report pipe
/// still works if it fails. Async-signal-safe: syscall(2) and fcntl(2) only.
void markInheritedFdsCloseOnExec(int fallbackLimit) noexcept
{
#if defined(SYS_close_range) && defined(CLOSE_RANGE_CLOEXEC)
    if (::syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) == 0)
    {
        return;
    }
#endif
    for (int fd = 3; fd < fallbackLimit; ++fd)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX fcntl() is variadic by definition
        static_cast<void>(::fcntl(fd, F_SETFD, FD_CLOEXEC));
    }
}

/// The detached program's process: reset what it would otherwise inherit from TaskSmack, then exec.
/// Never returns. Async-signal-safe calls only (signal(7)).
[[noreturn]] void execDetached(const char* path,
                               char* const* argv,
                               char* const* envp,
                               int devNull,
                               int reportFd,
                               int fallbackFdLimit,
                               const struct sigaction& defaultAction,
                               // NOLINTNEXTLINE(misc-include-cleaner) - sigset_t is provided by <signal.h>
                               const sigset_t& emptyMask) noexcept
{
    // Default dispositions: exec keeps ignored signals ignored (SIGPIPE, for one), which the terminal
    // and strace would then inherit. sigaction() refuses SIGKILL, SIGSTOP and libc's reserved signals;
    // that is harmless.
    for (int sig = 1; sig < NSIG; ++sig)
    {
        static_cast<void>(::sigaction(sig, &defaultAction, nullptr));
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - this forked child is single-threaded, and sigprocmask is async-signal-safe
    static_cast<void>(::sigprocmask(SIG_SETMASK, &emptyMask, nullptr));

    // stdin/stdout/stderr on /dev/null: the terminal draws its own window and must not write into
    // whatever TaskSmack was started from. dup2() clears close-on-exec on the copies.
    if (::dup2(devNull, STDIN_FILENO) < 0 || ::dup2(devNull, STDOUT_FILENO) < 0 || ::dup2(devNull, STDERR_FILENO) < 0)
    {
        writeReport(reportFd, SpawnFailure::Stage::Exec, errno);
        ::_exit(127);
    }
    markInheritedFdsCloseOnExec(fallbackFdLimit);

    ::execve(path, argv, envp);
    writeReport(reportFd, SpawnFailure::Stage::Exec, errno);
    ::_exit(127);
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
    char* const* const envp = environ;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const FdGuard devNull(::open("/dev/null", O_RDWR | O_CLOEXEC));
    if (devNull.get() < 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = errno});
    }
    std::array<int, 2> reportPipe{-1, -1};
    if (::pipe2(reportPipe.data(), O_CLOEXEC) != 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Setup, .error = errno});
    }
    const FdGuard reportRead(reportPipe[0]);
    FdGuard reportWrite(reportPipe[1]);

    int fallbackFdLimit = FALLBACK_FD_LIMIT;
    rlimit fileLimit{};
    if (::getrlimit(RLIMIT_NOFILE, &fileLimit) == 0 && fileLimit.rlim_cur != RLIM_INFINITY)
    {
        fallbackFdLimit = static_cast<int>(std::min<rlim_t>(fileLimit.rlim_cur, FALLBACK_FD_LIMIT));
    }
    struct sigaction defaultAction{};
    defaultAction.sa_handler = SIG_DFL;
    sigemptyset(&defaultAction.sa_mask);
    sigset_t emptyMask{};
    sigemptyset(&emptyMask);

    const char* const path = argPointers.front();
    const int devNullFd = devNull.get();
    const int reportFd = reportWrite.get();

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
            writeReport(reportFd, SpawnFailure::Stage::Fork, errno);
            ::_exit(1);
        }
        if (program == 0)
        {
            execDetached(path, argPointers.data(), envp, devNullFd, reportFd, fallbackFdLimit, defaultAction, emptyMask);
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

    pollfd pending{.fd = reportRead.get(), .events = POLLIN, .revents = 0};
    int ready = ::poll(&pending, 1, EXEC_REPORT_TIMEOUT_MS);
    while (ready < 0 && errno == EINTR)
    {
        ready = ::poll(&pending, 1, EXEC_REPORT_TIMEOUT_MS);
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
