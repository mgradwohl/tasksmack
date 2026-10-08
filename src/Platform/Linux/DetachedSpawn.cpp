#include "DetachedSpawn.h"

#include "PosixGuards.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

// NOLINTBEGIN(misc-include-cleaner) - POSIX headers: include-cleaner lacks mappings for pid_t, rlimit, wait macros
#include <dirent.h>
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

/// The most the whole call waits: for the detached child to report that execve() failed (or, by
/// closing the pipe, that it succeeded) and for the intermediate child to be reaped. An exec takes
/// milliseconds; the cap only bounds a pathological stall (a hung network filesystem) so the UI thread
/// is never held for long.
constexpr std::chrono::milliseconds SPAWN_TIMEOUT{2000};

/// How often the intermediate child is polled for with waitpid(WNOHANG) while it has not exited yet.
constexpr std::chrono::milliseconds REAP_POLL_INTERVAL{1};

/// Highest descriptor (exclusive) the last-resort fallback walks when RLIMIT_NOFILE is unlimited:
/// the kernel's default fs.nr_open, which no descriptor can exceed unless an administrator raised it.
constexpr int UNLIMITED_FD_LIMIT = 1 << 20;

/// What a child writes to the report pipe when it fails: the stage and errno.
struct Report
{
    std::int32_t stage = 0;
    std::int32_t error = 0;
};

/// Write @p report to @p fd. Async-signal-safe: only write(2), retried while a signal interrupts it
/// (otherwise the child would exit, the parent would see EOF, and a real failure would read as
/// success) and continued after a short write. The report is smaller than PIPE_BUF, so a pipe write
/// is all-or-nothing in practice; the loop just doesn't rely on it. Nothing useful can be done if
/// the write fails for any other reason.
void writeReport(int fd, SpawnFailure::Stage stage, int error) noexcept
{
    const Report report{.stage = static_cast<std::int32_t>(stage), .error = error};
    const auto bytes = std::as_bytes(std::span(&report, 1));
    std::size_t done = 0;
    while (done < bytes.size())
    {
        const auto written = ::write(fd, bytes.subspan(done).data(), bytes.size() - done);
        if (written > 0)
        {
            done += static_cast<std::size_t>(written);
        }
        else if (written < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return;
        }
    }
}

/// Mark @p fd close-on-exec. Async-signal-safe: fcntl(2) only. A descriptor that is not open fails
/// with EBADF, which is harmless.
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
    bool useProcFdList = true; ///< Whether the fallback may list /proc/self/fd in the child.
    int fdLimit = 0;           ///< Last resort when it can't: walk 3 .. fdLimit-1.
    const struct sigaction* defaultAction = nullptr;
    // NOLINTNEXTLINE(misc-include-cleaner) - sigset_t is provided by <signal.h>
    const sigset_t* emptyMask = nullptr;
};

/// @p name (a /proc/self/fd entry, NUL-terminated) as a descriptor, or -1 for "." and "..".
/// Async-signal-safe: no library calls.
[[nodiscard]] int parseFdName(std::span<const char> name) noexcept
{
    int fd = 0;
    bool any = false;
    for (const char c : name)
    {
        if (c == '\0')
        {
            break;
        }
        if (c < '0' || c > '9' || fd > (INT_MAX - 9) / 10)
        {
            return -1;
        }
        fd = (fd * 10) + (c - '0');
        any = true;
    }
    return any ? fd : -1;
}

/// Mark every descriptor above STDERR_FILENO listed in this process's /proc/self/fd close-on-exec.
/// Runs in the forked child, which has no other threads, so the list is exactly its descriptor table:
/// nothing can be opened between the listing and execve(). Async-signal-safe: open(2), the raw
/// getdents64(2) syscall into a stack buffer, fcntl(2) and close(2) -- no allocation. Returns false if
/// the directory could not be read to its end (no /proc, say).
[[nodiscard]] bool markListedFdsCloseOnExec() noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic by definition
    const int dir = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0)
    {
        return false;
    }
    // The kernel's linux_dirent64 records, as glibc's dirent64 lays them out.
    constexpr std::size_t RECLEN_OFFSET = offsetof(dirent64, d_reclen);
    constexpr std::size_t NAME_OFFSET = offsetof(dirent64, d_name);
    alignas(dirent64) std::array<char, 4096> buffer{};
    bool complete = false;
    for (;;)
    {
        const long got = ::syscall(SYS_getdents64, dir, buffer.data(), buffer.size());
        if (got <= 0)
        {
            complete = got == 0;
            break;
        }
        const auto size = static_cast<std::size_t>(got);
        for (std::size_t offset = 0; offset + NAME_OFFSET < size;)
        {
            unsigned short recordLength = 0;
            std::memcpy(&recordLength, std::span<const char>(buffer).subspan(offset + RECLEN_OFFSET).data(), sizeof(recordLength));
            if (recordLength <= NAME_OFFSET || offset + recordLength > size)
            {
                break;
            }
            const int fd = parseFdName(std::span<const char>(buffer).subspan(offset + NAME_OFFSET, recordLength - NAME_OFFSET));
            if (fd > STDERR_FILENO && fd != dir)
            {
                // The report pipe's write end is already close-on-exec; marking it again is harmless,
                // and it stays open until execve() so a failure can still be reported.
                setCloseOnExec(fd);
            }
            offset += recordLength;
        }
    }
    ::close(dir);
    return complete;
}

/// Mark every descriptor from 3 up close-on-exec, keeping them open until execve() so the report pipe
/// still works if it fails. Runs in the forked (single-threaded) child, so no descriptor can appear
/// after it. Async-signal-safe: syscall(2), open(2), fcntl(2) and close(2) only.
void markInheritedFdsCloseOnExec(const ChildPlan& plan) noexcept
{
#if defined(SYS_close_range) && defined(CLOSE_RANGE_CLOEXEC)
    if (plan.useCloseRange && ::syscall(SYS_close_range, 3U, ~0U, CLOSE_RANGE_CLOEXEC) == 0)
    {
        return;
    }
#endif
    if (plan.useProcFdList && markListedFdsCloseOnExec())
    {
        return;
    }
    // Last resort: every descriptor RLIMIT_NOFILE allows (none can be at or above it).
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

/// The milliseconds left until @p deadline for poll(2): 0 once it has passed, never more than the cap.
[[nodiscard]] int remainingMs(std::chrono::steady_clock::time_point deadline) noexcept
{
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    return left.count() <= 0 ? 0 : static_cast<int>(std::min(left, SPAWN_TIMEOUT).count());
}

/// Reap @p child, polling with waitpid(WNOHANG) until @p deadline. If it has not exited by then it is
/// killed (SIGKILL) and reaped, so it is never left as a zombie, and false is returned.
[[nodiscard]] bool reapBy(pid_t child, std::chrono::steady_clock::time_point deadline, const std::string& program)
{
    for (;;)
    {
        int status = 0;
        // NOLINTNEXTLINE(misc-include-cleaner) - WNOHANG is provided by <sys/wait.h>
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) // ECHILD: SIGCHLD ignored, already reaped
        {
            return true;
        }
        if (waited < 0 && errno != EINTR)
        {
            spdlog::warn(
                "DetachedSpawn: waitpid for the intermediate child of {} failed: {}", program, std::system_category().message(errno));
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }
        std::this_thread::sleep_for(REAP_POLL_INTERVAL);
    }
    spdlog::warn("DetachedSpawn: the intermediate child of {} did not exit within {} ms; killing it", program, SPAWN_TIMEOUT.count());
    static_cast<void>(::kill(child, SIGKILL));
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
    {
    }
    return false;
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
        .useProcFdList = hooks.useProcFdList,
        .fdLimit = descriptorLimit(),
        .defaultAction = &defaultAction,
        .emptyMask = &emptyMask,
    };

    const auto setsidFn = hooks.setsid != nullptr ? hooks.setsid : &::setsid;
    if (hooks.beforeFork != nullptr)
    {
        hooks.beforeFork();
    }

    const pid_t intermediate = ::fork();
    if (intermediate < 0)
    {
        return std::unexpected(SpawnFailure{.stage = SpawnFailure::Stage::Fork, .error = errno});
    }
    if (intermediate == 0)
    {
        // TaskSmack's child: a session of its own, so the terminal is not in TaskSmack's process group
        // and session (a Ctrl+C or hangup aimed at those does not reach it); then fork the program's
        // process and exit, orphaning it to init. Without the new session it is not detached, so a
        // failure is reported rather than ignored.
        if (setsidFn() < 0)
        {
            writeReport(plan.reportFd, SpawnFailure::Stage::Setup, errno);
            ::_exit(1);
        }
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

    // One deadline bounds the whole wait: the poll for the report (retried on EINTR with only the time
    // left), then reaping the intermediate child, which exits as soon as it has forked.
    const auto deadline = std::chrono::steady_clock::now() + SPAWN_TIMEOUT;
    const std::string& program = argv.front();

    // read() below runs only once the pipe is readable (a report, or end of file), so it never blocks.
    const auto pollFn = hooks.poll != nullptr ? hooks.poll : &::poll;
    pollfd pending{.fd = reportRead.get(), .events = POLLIN, .revents = 0};
    int ready = pollFn(&pending, 1, remainingMs(deadline));
    while (ready < 0 && errno == EINTR)
    {
        ready = pollFn(&pending, 1, remainingMs(deadline));
    }
    const int pollError = errno;

    std::optional<SpawnFailure> reported;
    if (ready > 0)
    {
        Report report{};
        auto got = ::read(reportRead.get(), &report, sizeof(report));
        while (got < 0 && errno == EINTR)
        {
            got = ::read(reportRead.get(), &report, sizeof(report));
        }
        // Otherwise end of file: exec succeeded. A short read cannot happen (the report is written whole).
        if (std::cmp_equal(got, sizeof(report)))
        {
            reported = SpawnFailure{.stage = static_cast<SpawnFailure::Stage>(report.stage), .error = report.error};
        }
    }

    // The only wait, and it is not on the program. Bounded by the same deadline.
    if (!reapBy(intermediate, deadline, program))
    {
        // Stuck before it could fork the program (or report why it couldn't): nothing was started.
        return std::unexpected(reported.value_or(SpawnFailure{.stage = SpawnFailure::Stage::Fork, .error = ETIMEDOUT}));
    }
    if (reported)
    {
        return std::unexpected(*reported);
    }
    if (ready < 0)
    {
        // As for a timeout: the program may still exec, so it is not reported as failed (the user
        // would start a second one), and a blocking read() here could hold the UI thread indefinitely.
        spdlog::warn("DetachedSpawn: poll for the exec report of {} failed ({}); assuming it started",
                     program,
                     std::system_category().message(pollError));
    }
    else if (ready == 0)
    {
        spdlog::warn("DetachedSpawn: {} has not reported its exec after {} ms; assuming it started", program, SPAWN_TIMEOUT.count());
    }
    return {};
}

} // namespace Platform::DetachedSpawn
