#include "PolkitTextAgent.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX signal.h provides kill() and sigset_t, csignal does not
#include <spawn.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ; // NOLINT(readability-redundant-declaration,cppcoreguidelines-avoid-non-const-global-variables) - POSIX

namespace Platform
{

namespace
{

/// Whether @p fd reads EOF (the child registered, or exited) within @p timeout, before @p stopToken is signalled
/// (looked at every PolkitTextAgent::STOP_POLL), retrying an interrupted wait.
[[nodiscard]] bool waitForReady(int fd, std::chrono::milliseconds timeout, const std::stop_token& stopToken) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!stopToken.stop_requested())
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0)
        {
            return false;
        }
        pollfd readable{.fd = fd, .events = POLLIN, .revents = 0};
        const int result = ::poll(&readable, 1, static_cast<int>(std::min(left, PolkitTextAgent::STOP_POLL).count()));
        if (result > 0)
        {
            return true;
        }
        if (result < 0 && errno != EINTR)
        {
            return false;
        }
    }
    return false;
}

/// Reaps @p pid if it has exited within @p window: the ready descriptor closes when the child exits, too, and an
/// exiting process can't be reaped quite yet.
// NOLINTNEXTLINE(misc-include-cleaner) - pid_t is <sys/types.h>'s, which PolkitTextAgent.h includes; the tool maps it oddly
[[nodiscard]] bool exitsWithin(pid_t pid, std::chrono::milliseconds window) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + window;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (::waitpid(pid, nullptr, WNOHANG) == pid) // NOLINT(misc-include-cleaner) - WNOHANG is <sys/wait.h>'s
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

std::unique_ptr<PolkitTextAgent> PolkitTextAgent::start(const std::stop_token& stopToken, std::string& error)
{
    return startWith(
        {"/proc/self/exe", AGENT_ARGUMENT}, std::chrono::duration_cast<std::chrono::milliseconds>(REGISTER_TIMEOUT), stopToken, error);
}

std::unique_ptr<PolkitTextAgent> PolkitTextAgent::startWith(const std::vector<std::string>& command,
                                                            std::chrono::milliseconds timeout,
                                                            const std::stop_token& stopToken,
                                                            std::string& error)
{
    if (command.empty())
    {
        error = "no agent to start";
        return nullptr;
    }
    // Built before anything is opened, so nothing after it allocates and can leak a descriptor.
    std::vector<std::string> arguments = command;
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::unique_ptr<PolkitTextAgent> agent(new PolkitTextAgent()); // NOLINT(cppcoreguidelines-owning-memory) - private constructor

    // The terminal's settings, put back when the agent stops: one killed mid-prompt leaves echo off.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) - POSIX open() is variadic
    agent->m_Tty = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    agent->m_TermiosSaved = agent->m_Tty >= 0 && ::tcgetattr(agent->m_Tty, &agent->m_Termios) == 0;

    std::array<int, 2> ready{-1, -1};
    if (::pipe2(ready.data(), O_CLOEXEC) != 0)
    {
        error = "couldn't make the agent's pipe";
        return nullptr;
    }
    posix_spawn_file_actions_t files{};
    bool prepared = posix_spawn_file_actions_init(&files) == 0;
    prepared = prepared && posix_spawn_file_actions_adddup2(&files, ready[1], READY_FD) == 0;
    const int spawned = prepared ? ::posix_spawn(&agent->m_Pid, argv.front(), &files, nullptr, argv.data(), environ) : -1;
    posix_spawn_file_actions_destroy(&files);
    ::close(ready[1]);
    if (spawned != 0)
    {
        agent->m_Pid = -1;
        ::close(ready[0]);
        error = "couldn't start the agent";
        return nullptr;
    }
    const bool closed = waitForReady(ready[0], timeout, stopToken);
    ::close(ready[0]);
    // It closes the descriptor when it registers and when it exits: one still running a moment later registered.
    if (!closed || exitsWithin(agent->m_Pid, std::chrono::milliseconds(200)))
    {
        if (!closed)
        {
            error = stopToken.stop_requested() ? "stopped while the agent registered" : "the agent didn't register in time";
        }
        else
        {
            agent->m_Pid = -1; // reaped
            error = "the agent couldn't register (no controlling terminal, or polkit refused)";
        }
        return nullptr; // the destructor stops one still running
    }
    return agent;
}

PolkitTextAgent::~PolkitTextAgent()
{
    if (m_Pid > 0)
    {
        // A stopped child acts on no SIGTERM: continued first; one that still hasn't gone in a second is killed.
        static_cast<void>(::kill(m_Pid, SIGCONT));
        static_cast<void>(::kill(m_Pid, SIGTERM));
        if (!exitsWithin(m_Pid, std::chrono::seconds(1)))
        {
            static_cast<void>(::kill(m_Pid, SIGKILL));
            static_cast<void>(::waitpid(m_Pid, nullptr, 0));
        }
    }
    if (m_TermiosSaved && ::tcgetpgrp(m_Tty) == ::getpgrp())
    {
        // Only while still in the foreground, and with SIGTTOU blocked on this thread: from the background,
        // tcsetattr() would stop the whole process group, the window with it.
        sigset_t ttou{}; // NOLINT(misc-include-cleaner) - sigset_t is <signal.h>'s
        sigset_t previous{};
        sigemptyset(&ttou);
        sigaddset(&ttou, SIGTTOU);
        const bool blocked = ::pthread_sigmask(SIG_BLOCK, &ttou, &previous) == 0;
        static_cast<void>(::tcsetattr(m_Tty, TCSANOW, &m_Termios));
        if (blocked)
        {
            static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
        }
    }
    if (m_Tty >= 0)
    {
        ::close(m_Tty);
    }
}

} // namespace Platform
