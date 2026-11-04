#include "CommandOutput.h"

#include "PosixGuards.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <string>
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

CommandOutput runForOutput(const std::vector<std::string>& argv, std::chrono::milliseconds timeout, std::size_t maxBytes)
{
    CommandOutput result;
    if (argv.empty())
    {
        return result;
    }
    std::array<int, 2> pipeFds{-1, -1};
    if (::pipe2(pipeFds.data(), O_CLOEXEC) != 0)
    {
        return result;
    }
    const Posix::FdGuard readEnd(pipeFds[0]);
    Posix::FdGuard writeEnd(pipeFds[1]);

    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    ::posix_spawn_file_actions_init(&actions);
    ::posix_spawnattr_init(&attributes);
    ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2(&actions, writeEnd.get(), STDOUT_FILENO);
    ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    ::posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + 1); // nothing else of TaskSmack's
#endif
    // An empty signal mask and default dispositions, whatever this thread blocks or ignores.
    sigset_t signals{}; // NOLINT(misc-include-cleaner) - sigset_t is provided by <signal.h>
    sigemptyset(&signals);
    ::posix_spawnattr_setsigmask(&attributes, &signals);
    sigaddset(&signals, SIGPIPE);
    ::posix_spawnattr_setsigdefault(&attributes, &signals);
    ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& arg : argv)
    {
        args.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast) - posix_spawn's char* const[]
    }
    args.push_back(nullptr);
    pid_t child = -1; // NOLINT(misc-include-cleaner) - from <sys/types.h>, which clang-tidy 23 doesn't map
    const int spawned = ::posix_spawnp(&child, argv.front().c_str(), &actions, &attributes, args.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    ::posix_spawnattr_destroy(&attributes);
    {
        const Posix::FdGuard closing(std::move(writeEnd)); // only the child writes: end of file once it is gone
    }
    if (spawned != 0)
    {
        return result;
    }
    result.started = true;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> chunk{};
    for (;;)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
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
    if (result.timedOut)
    {
        ::kill(child, SIGKILL);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
    {
    }
    if (!result.timedOut && WIFEXITED(status)) // NOLINT(misc-include-cleaner) - from <sys/wait.h>
    {
        result.exitCode = WEXITSTATUS(status); // NOLINT(misc-include-cleaner) - from <sys/wait.h>
    }
    return result;
}

} // namespace Platform
