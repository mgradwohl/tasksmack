#include "CommandOutput.h"

#include "PosixGuards.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <features.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX sigemptyset, sigaddset and kill, which <csignal> lacks
#include <spawn.h>
#include <sys/poll.h>
#include <sys/types.h> // NOLINT(misc-include-cleaner) - pid_t; clang-tidy 23 maps it to no header
#include <sys/wait.h>
#include <unistd.h>

extern char**
    environ; // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables) - POSIX: the process environment, for posix_spawn

namespace Platform
{

namespace
{

/// How often a child that has closed its output is checked for having exited, until the deadline.
constexpr std::chrono::milliseconds EXIT_POLL_STEP{5};

/// Whether a test asked for the listed closes even where posix_spawn_file_actions_addclosefrom_np() is there.
[[nodiscard]] std::atomic<bool>& listedClosesForTesting() noexcept
{
    static std::atomic<bool> on{false};
    return on;
}

/// Every descriptor above STDERR_FILENO this process has open now, from /proc/self/fd: what the child would inherit
/// of those opened without O_CLOEXEC (#3762: libbpf's or sd-bus's, as root ones only root could open). Empty when
/// /proc can't be read. A descriptor another thread opens after the listing isn't in it, and one it closes since is
/// harmless: glibc's posix_spawn ignores a close of a descriptor that is no longer open.
[[nodiscard]] std::vector<int> openDescriptorsAboveStderr()
{
    std::vector<int> fds;
    std::error_code ec;
    for (std::filesystem::directory_iterator entry("/proc/self/fd", ec), end; !ec && entry != end; entry.increment(ec))
    {
        const std::string name = entry->path().filename().string();
        int fd = -1;
        const auto [last, error] = std::from_chars(name.data(), name.data() + name.size(), fd);
        if (error == std::errc{} && last == name.data() + name.size() && fd > STDERR_FILENO)
        {
            fds.push_back(fd); // the listing's own descriptor too, which is closed by then: harmless, as above
        }
    }
    return fds;
}

/// Closes in the child every descriptor of TaskSmack's above stderr: with posix_spawn_file_actions_addclosefrom_np()
/// where glibc has it (2.34 and later), else one close per descriptor /proc/self/fd lists (#3762).
void addCloseEverythingAboveStderr(posix_spawn_file_actions_t* actions)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    if (!listedClosesForTesting().load(std::memory_order_relaxed))
    {
        ::posix_spawn_file_actions_addclosefrom_np(actions, STDERR_FILENO + 1);
        return;
    }
#endif
    for (const int fd : openDescriptorsAboveStderr())
    {
        ::posix_spawn_file_actions_addclose(actions, fd);
    }
}

/// posix_spawn's file actions, destroyed on every path once initialised (an allocation below may throw).
class SpawnFileActions
{
  public:
    SpawnFileActions() noexcept : m_Ready(::posix_spawn_file_actions_init(&m_Actions) == 0)
    {}
    ~SpawnFileActions()
    {
        if (m_Ready)
        {
            ::posix_spawn_file_actions_destroy(&m_Actions);
        }
    }
    SpawnFileActions(const SpawnFileActions&) = delete;
    SpawnFileActions& operator=(const SpawnFileActions&) = delete;
    SpawnFileActions(SpawnFileActions&&) = delete;
    SpawnFileActions& operator=(SpawnFileActions&&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return m_Ready;
    }
    [[nodiscard]] posix_spawn_file_actions_t* get() noexcept
    {
        return &m_Actions;
    }

  private:
    posix_spawn_file_actions_t m_Actions{};
    bool m_Ready;
};

/// posix_spawn's attributes, destroyed on every path once initialised.
class SpawnAttributes
{
  public:
    SpawnAttributes() noexcept : m_Ready(::posix_spawnattr_init(&m_Attributes) == 0)
    {}
    ~SpawnAttributes()
    {
        if (m_Ready)
        {
            ::posix_spawnattr_destroy(&m_Attributes);
        }
    }
    SpawnAttributes(const SpawnAttributes&) = delete;
    SpawnAttributes& operator=(const SpawnAttributes&) = delete;
    SpawnAttributes(SpawnAttributes&&) = delete;
    SpawnAttributes& operator=(SpawnAttributes&&) = delete;

    [[nodiscard]] bool ready() const noexcept
    {
        return m_Ready;
    }
    [[nodiscard]] posix_spawnattr_t* get() noexcept
    {
        return &m_Attributes;
    }

  private:
    posix_spawnattr_t m_Attributes{};
    bool m_Ready;
};

/// Waits for @p child without blocking. Sets @p reaped and returns its status once it has exited; returns nullopt
/// while it still runs. A failed wait (ECHILD: nothing left to wait for) sets @p reaped with no status.
// NOLINTNEXTLINE(misc-include-cleaner) - pid_t is from <sys/types.h>, which clang-tidy 23 doesn't map
[[nodiscard]] std::optional<int> reapIfExited(pid_t child, bool& reaped) noexcept
{
    int status = 0;
    for (;;)
    {
        // NOLINTNEXTLINE(misc-include-cleaner) - pid_t from <sys/types.h>, WNOHANG from <sys/wait.h>
        const pid_t got = ::waitpid(child, &status, WNOHANG);
        if (got == child)
        {
            reaped = true;
            return status;
        }
        if (got == 0)
        {
            return std::nullopt;
        }
        if (errno != EINTR)
        {
            reaped = true; // nothing left to wait for
            return std::nullopt;
        }
    }
}

/// Owns a spawned child until it is reaped: on any early exit, an exception included, kills it (SIGKILL) and reaps
/// it, so no process is left running unwatched and no zombie stays behind.
class ChildGuard
{
  public:
    explicit ChildGuard(pid_t child) noexcept : m_Child(child)
    {}
    ~ChildGuard()
    {
        if (m_Child > 0)
        {
            ::kill(m_Child, SIGKILL);
            int status = 0;
            while (::waitpid(m_Child, &status, 0) < 0 && errno == EINTR)
            {
            }
        }
    }
    ChildGuard(const ChildGuard&) = delete;
    ChildGuard& operator=(const ChildGuard&) = delete;
    ChildGuard(ChildGuard&&) = delete;
    ChildGuard& operator=(ChildGuard&&) = delete;

    [[nodiscard]] pid_t get() const noexcept
    {
        return m_Child;
    }
    /// It has been reaped: nothing left to do.
    void release() noexcept
    {
        m_Child = -1;
    }

  private:
    pid_t m_Child; // NOLINT(misc-include-cleaner) - from <sys/types.h>, which clang-tidy 23 doesn't map
};

} // namespace

CommandOutput runForOutput(const std::vector<std::string>& argv,
                           std::chrono::milliseconds timeout,
                           std::size_t maxBytes,
                           const std::vector<std::string>* environment)
{
    CommandOutput result;
    if (argv.empty())
    {
        return result;
    }
    std::vector<char*> args; // made first, with env below: the allocations before the spawn
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
    {
        args.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast) - posix_spawn's char* const[]
    }
    args.push_back(nullptr);
    std::vector<char*> env; // the given environment, when there is one; made before the spawn too
    if (environment != nullptr)
    {
        env.reserve(environment->size() + 1);
        for (const std::string& entry : *environment)
        {
            env.push_back(const_cast<char*>(entry.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast) - posix_spawn's char* const[]
        }
        env.push_back(nullptr);
    }

    std::array<int, 2> pipeFds{-1, -1};
    if (::pipe2(pipeFds.data(), O_CLOEXEC) != 0)
    {
        return result;
    }
    const Posix::FdGuard readEnd(pipeFds[0]);
    Posix::FdGuard writeEnd(pipeFds[1]);

    SpawnFileActions actions;
    SpawnAttributes attributes;
    if (!actions.ready() || !attributes.ready())
    {
        return result;
    }
    ::posix_spawn_file_actions_addopen(actions.get(), STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2(actions.get(), writeEnd.get(), STDOUT_FILENO);
    ::posix_spawn_file_actions_addopen(actions.get(), STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    addCloseEverythingAboveStderr(actions.get()); // nothing else of TaskSmack's
    // An empty signal mask and default dispositions, whatever this thread blocks or ignores.
    sigset_t signals{}; // NOLINT(misc-include-cleaner) - sigset_t is provided by <signal.h>
    sigemptyset(&signals);
    ::posix_spawnattr_setsigmask(attributes.get(), &signals);
    sigaddset(&signals, SIGPIPE);
    ::posix_spawnattr_setsigdefault(attributes.get(), &signals);
    ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t spawnedChild = -1; // NOLINT(misc-include-cleaner) - from <sys/types.h>, which clang-tidy 23 doesn't map
    const int spawned = ::posix_spawnp(
        &spawnedChild, argv.front().c_str(), actions.get(), attributes.get(), args.data(), environment != nullptr ? env.data() : environ);
    {
        const Posix::FdGuard closing(std::move(writeEnd)); // only the child writes: end of file once it is gone
    }
    if (spawned != 0)
    {
        return result;
    }
    ChildGuard child(spawnedChild); // from here, every way out kills and reaps it unless it was reaped below
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto timeLeft = [deadline]
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    };
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const std::chrono::milliseconds left = timeLeft();
        if (left.count() <= 0)
        {
            result.timedOut = true;
            break;
        }
        pollfd ready{.fd = readEnd.get(), .events = POLLIN, .revents = 0};
        const int polled = ::poll(&ready, 1, static_cast<int>(left.count()));
        if (polled < 0 && errno == EINTR)
        {
            continue;
        }
        if (polled <= 0)
        {
            result.timedOut = polled == 0;
            break;
        }
        // NOLINTNEXTLINE(misc-include-cleaner) - ssize_t is from <sys/types.h>, which clang-tidy 23 doesn't map
        const ssize_t got = ::read(readEnd.get(), chunk.data(), chunk.size());
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got <= 0)
        {
            break; // end of file: it closed its output, normally by exiting
        }
        const std::size_t room = maxBytes > result.output.size() ? maxBytes - result.output.size() : 0;
        result.output.append(chunk.data(), std::min(room, static_cast<std::size_t>(got)));
    }

    // Closing its output isn't exiting (`exec >&-; sleep 30`): it gets until the same deadline to exit, checked
    // without blocking, and is killed (ChildGuard) if it hasn't by then.
    std::optional<int> status;
    bool reaped = false;
    while (!result.timedOut)
    {
        status = reapIfExited(child.get(), reaped);
        if (reaped)
        {
            break;
        }
        const std::chrono::milliseconds left = timeLeft();
        if (left.count() <= 0)
        {
            result.timedOut = true;
            break;
        }
        std::this_thread::sleep_for(std::min(left, EXIT_POLL_STEP));
    }
    if (reaped)
    {
        child.release();
    }
    if (!result.timedOut && status.has_value() && WIFEXITED(*status)) // NOLINT(misc-include-cleaner) - from <sys/wait.h>
    {
        result.exitCode = WEXITSTATUS(*status); // NOLINT(misc-include-cleaner) - from <sys/wait.h>
    }
    return result; // a child still running here (timed out, or an error) is killed and reaped by ChildGuard
}

namespace Detail
{

void useListedClosesForTesting(bool on) noexcept
{
    listedClosesForTesting().store(on, std::memory_order_relaxed);
}

} // namespace Detail

} // namespace Platform
