#pragma once

// polkit's text authentication agent, registered in TaskSmack's own process as the user (#3380). Where no desktop
// polkit agent runs (WSL, a session without a display), pkexec falls back to a text agent of its own, which runs as
// root; polkit 127 checks the password in a helper that answers as the user who connects to it, and polkitd takes the
// answer only for an agent that user registered, so the right password was refused with "No session for cookie"
// (polkit-org/polkit#686). Registered here, as the user, for this process (the subject pkexec asks about, being its
// parent), the agent asks on the controlling terminal and the answer is taken. pkttyagent run as a separate program
// doesn't do: polkitd finds no agent for pkexec's request from either its --process or its session registration.

#include <memory>
#include <string>

namespace Platform
{

class PolkitTextAgent
{
  public:
    /// Registers the agent for this process, asking on its controlling terminal, until destroyed. nullptr, with @p error
    /// saying why, when it can't: TaskSmack built without libpolkit-agent-1, no controlling terminal, polkitd refused.
    [[nodiscard]] static std::unique_ptr<PolkitTextAgent> registerForThisProcess(std::string& error);

    ~PolkitTextAgent();
    PolkitTextAgent(const PolkitTextAgent&) = delete;
    PolkitTextAgent& operator=(const PolkitTextAgent&) = delete;
    PolkitTextAgent(PolkitTextAgent&&) = delete;
    PolkitTextAgent& operator=(PolkitTextAgent&&) = delete;

  private:
    PolkitTextAgent(void* listener, void* registration) noexcept : m_Listener(listener), m_Registration(registration)
    {}

    void* m_Listener = nullptr;     ///< PolkitAgentListener*, a GObject
    void* m_Registration = nullptr; ///< polkit_agent_listener_register()'s handle
};

} // namespace Platform
