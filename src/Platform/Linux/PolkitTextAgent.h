#pragma once

// polkit's text authentication agent for TaskSmack's password prompt, run as the user in a child process (#3380).
// Where no desktop polkit agent runs (WSL, a session without a display), pkexec falls back to a text agent of its
// own, which runs as root; polkit 127 checks the password in a helper that answers as the user who connects to it, and
// polkitd takes the answer only for an agent that user registered, so the right password was refused with "No session
// for cookie" (polkit-org/polkit#686). The child, TaskSmack itself started with AGENT_ARGUMENT, registers polkit's text
// listener for its parent (the process pkexec asks about, pkexec being its child too), described as pkexec describes
// it, and asks on the controlling terminal. pkttyagent doesn't do: polkitd finds no agent for pkexec's request from its
// registration. A child rather than a thread, because the listener blocks in a terminal read that nothing can
// interrupt, and unregistering waits for it: a prompt nobody answers would hang TaskSmack, where a child is killed.

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <sys/types.h>
#include <termios.h>

namespace Platform
{

class PolkitTextAgent
{
  public:
    /// The argument TaskSmack's main() answers by running the agent (runAgent()) instead of the app.
    static constexpr const char* AGENT_ARGUMENT = "--polkit-text-agent";
    /// The descriptor the child closes once it has registered.
    static constexpr int READY_FD = 3;
    /// How long start() waits for the child to register.
    static constexpr std::chrono::seconds REGISTER_TIMEOUT{5};

    /// Starts the agent for this process: TaskSmack itself (/proc/self/exe) with AGENT_ARGUMENT, until destroyed.
    /// nullptr, with @p error saying why, when it isn't running and registered within REGISTER_TIMEOUT, or when
    /// @p stopToken is signalled first (the wait looks every STOP_POLL); the child is stopped then.
    [[nodiscard]] static std::unique_ptr<PolkitTextAgent> start(const std::stop_token& stopToken, std::string& error);
    /// start() with @p command as the child (the tests: a fake that closes READY_FD, or doesn't), waiting @p timeout.
    [[nodiscard]] static std::unique_ptr<PolkitTextAgent> startWith(const std::vector<std::string>& command,
                                                                    std::chrono::milliseconds timeout,
                                                                    const std::stop_token& stopToken,
                                                                    std::string& error);
    /// How often the wait for the child to register looks at the stop token.
    static constexpr std::chrono::milliseconds STOP_POLL{100};

    /// The child's side, from main(): registers the text listener for the parent, closes READY_FD, and serves until
    /// killed or the parent goes. The process's exit status: nonzero when it couldn't register (no controlling
    /// terminal, TaskSmack built without libpolkit-agent-1, polkitd refused).
    [[nodiscard]] static int runAgent();

    /// Stops the child (SIGTERM, then SIGKILL after a second: never waits longer) and puts back the terminal's
    /// settings, which a prompt cut short leaves with echo off.
    ~PolkitTextAgent();
    PolkitTextAgent(const PolkitTextAgent&) = delete;
    PolkitTextAgent& operator=(const PolkitTextAgent&) = delete;
    PolkitTextAgent(PolkitTextAgent&&) = delete;
    PolkitTextAgent& operator=(PolkitTextAgent&&) = delete;

  private:
    PolkitTextAgent() = default;

    pid_t m_Pid = -1;
    int m_Tty = -1;
    bool m_TermiosSaved = false;
    termios m_Termios{};
};

} // namespace Platform
