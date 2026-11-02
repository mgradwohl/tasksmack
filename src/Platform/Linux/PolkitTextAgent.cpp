#include "PolkitTextAgent.h"

#include <memory>
#include <string>

#ifdef TASKSMACK_HAS_POLKIT_AGENT
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - the library asks for this define before its header
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#include <unistd.h>
#endif

namespace Platform
{

#ifdef TASKSMACK_HAS_POLKIT_AGENT

// polkit's only supported include is its umbrella header, which brings GLib and the polkit types used below.
// NOLINTBEGIN(misc-include-cleaner)
std::unique_ptr<PolkitTextAgent> PolkitTextAgent::registerForThisProcess(std::string& error)
{
    GError* failure = nullptr;
    // As pkexec builds the subject it asks about, for its parent: this process, its start time looked up, owned by us.
    PolkitSubject* subject = polkit_unix_process_new_for_owner(::getpid(), 0, static_cast<gint>(::getuid()));
    PolkitAgentListener* listener = polkit_agent_text_listener_new(nullptr, &failure);
    if (listener == nullptr)
    {
        error = failure != nullptr ? failure->message : "polkit's text agent couldn't be made";
        g_clear_error(&failure);
        g_object_unref(subject);
        return nullptr;
    }
    // Its own thread runs the agent's main loop, so the password is asked while pkexec waits.
    gpointer registration =
        polkit_agent_listener_register(listener, POLKIT_AGENT_REGISTER_FLAGS_RUN_IN_THREAD, subject, nullptr, nullptr, &failure);
    g_object_unref(subject);
    if (registration == nullptr)
    {
        error = failure != nullptr ? failure->message : "polkit's text agent couldn't be registered";
        g_clear_error(&failure);
        g_object_unref(listener);
        return nullptr;
    }
    return std::unique_ptr<PolkitTextAgent>(
        new PolkitTextAgent(listener, registration)); // NOLINT(cppcoreguidelines-owning-memory) - private constructor
}

PolkitTextAgent::~PolkitTextAgent()
{
    if (m_Registration != nullptr)
    {
        polkit_agent_listener_unregister(m_Registration);
    }
    if (m_Listener != nullptr)
    {
        g_object_unref(m_Listener);
    }
}
// NOLINTEND(misc-include-cleaner)

#else

std::unique_ptr<PolkitTextAgent> PolkitTextAgent::registerForThisProcess(std::string& error)
{
    error = "TaskSmack was built without libpolkit-agent-1";
    return nullptr;
}

PolkitTextAgent::~PolkitTextAgent()
{
    // Never made without the library; named so a build without it has no unused fields.
    static_cast<void>(m_Listener);
    static_cast<void>(m_Registration);
}

#endif

} // namespace Platform
