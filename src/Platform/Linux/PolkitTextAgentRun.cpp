// The polkit text agent's own side (#3380, PolkitTextAgent.h): TaskSmack started with AGENT_ARGUMENT, its main()
// calling runAgent(). Apart from PolkitTextAgent.cpp so that only this file brings in polkit's (and GLib's) headers.

#include "PolkitTextAgent.h"

#include <cstdlib>

#include <signal.h> // NOLINT(modernize-deprecated-headers) - POSIX signal.h provides SIGTERM for prctl, csignal does not
#include <sys/prctl.h>
#include <unistd.h>

#ifdef TASKSMACK_HAS_POLKIT_AGENT
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage) - the library asks for this define before its header
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#endif

namespace Platform
{

#ifdef TASKSMACK_HAS_POLKIT_AGENT

// polkit's only supported include is its umbrella header, which brings GLib and the polkit types used below.
// NOLINTBEGIN(misc-include-cleaner)
int PolkitTextAgent::runAgent()
{
    // Gone with TaskSmack: a prompt left behind by a TaskSmack that died would ask for nothing.
    static_cast<void>(::prctl(PR_SET_PDEATHSIG, SIGTERM));
    const pid_t parent = ::getppid();
    if (parent <= 1)
    {
        return EXIT_FAILURE; // the parent is already gone
    }
    GError* failure = nullptr;
    // As pkexec builds the subject it asks about, its parent: TaskSmack, its start time looked up, owned by us.
    PolkitSubject* subject = polkit_unix_process_new_for_owner(parent, 0, static_cast<gint>(::getuid()));
    PolkitAgentListener* listener = polkit_agent_text_listener_new(nullptr, &failure);
    if (listener == nullptr)
    {
        g_clear_error(&failure);
        g_object_unref(subject);
        return EXIT_FAILURE;
    }
    gpointer registration = polkit_agent_listener_register(listener, POLKIT_AGENT_REGISTER_FLAGS_NONE, subject, nullptr, nullptr, &failure);
    g_object_unref(subject);
    if (registration == nullptr)
    {
        g_clear_error(&failure);
        g_object_unref(listener);
        return EXIT_FAILURE;
    }
    ::close(READY_FD); // registered
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop); // until killed
    g_main_loop_unref(loop);
    polkit_agent_listener_unregister(registration);
    g_object_unref(listener);
    return EXIT_SUCCESS;
}
// NOLINTEND(misc-include-cleaner)

#else

int PolkitTextAgent::runAgent()
{
    return EXIT_FAILURE; // built without libpolkit-agent-1: no agent, and pkexec's own asks
}

#endif

} // namespace Platform
