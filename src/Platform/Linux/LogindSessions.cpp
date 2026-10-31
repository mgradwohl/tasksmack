// logind's sessions for the Users tab (#3076): ListSessions, then each session's properties. The actions
// (LockSession, TerminateSession) are in SystemdUnitActions.cpp, with the other calls that may prompt.

#include "Platform/IUserSessions.h"
#include "Platform/Linux/SystemdBus.h"

#include <string>
#include <string_view>

#ifdef TASKSMACK_HAS_SDBUS

#include "Platform/Linux/SdBusCall.h"

#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

// See SdBusCall.h: where sd_bus_message is declared depends on the libsystemd version.
#include <systemd/sd-bus-protocol.h> // NOLINT(misc-include-cleaner) - see above
#include <systemd/sd-bus.h>

#endif

namespace Platform::SystemdBus
{

#ifdef TASKSMACK_HAS_SDBUS

namespace
{

using Detail::Bus;
using Detail::BusError;
using Detail::Message;

constexpr const char* LOGIN1 = "org.freedesktop.login1";
constexpr const char* LOGIN1_PATH = "/org/freedesktop/login1";
constexpr const char* LOGIN1_MANAGER = "org.freedesktop.login1.Manager";
constexpr const char* LOGIN1_SESSION = "org.freedesktop.login1.Session";

/// A string property of session object @p path; empty when it can't be read.
[[nodiscard]] std::string sessionString(sd_bus* bus, const std::string& path, const char* property)
{
    BusError error;
    char* value = nullptr;
    std::string text;
    if (sd_bus_get_property_string(bus, LOGIN1, path.c_str(), LOGIN1_SESSION, property, error.get(), &value) >= 0 && value != nullptr)
    {
        text = value;
    }
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory) - sd-bus hands back malloc'd strings
    return text;
}

/// A trivial property ('b', 'u', 't') of session object @p path into @p value; false when it can't be read.
template<typename T> bool sessionTrivial(sd_bus* bus, const std::string& path, const char* property, char type, T& value)
{
    BusError error;
    return sd_bus_get_property_trivial(bus, LOGIN1, path.c_str(), LOGIN1_SESSION, property, error.get(), type, &value) >= 0;
}

/// The session at @p path, its ID, user and seat already read from ListSessions.
[[nodiscard]] UserSession readSession(sd_bus* bus, UserSession session, const std::string& path)
{
    session.tty = sessionString(bus, path, "TTY");
    session.type = sessionString(bus, path, "Type");
    session.sessionClass = sessionString(bus, path, "Class");
    session.state = sessionString(bus, path, "State");
    session.service = sessionString(bus, path, "Service");
    session.remoteHost = sessionString(bus, path, "RemoteHost");
    int remote = 0; // sd-bus reads a 'b' into an int
    if (sessionTrivial(bus, path, "Remote", 'b', remote))
    {
        session.remote = remote != 0;
    }
    int idle = 0;
    if (sessionTrivial(bus, path, "IdleHint", 'b', idle))
    {
        session.idle = idle != 0;
    }
    std::uint64_t idleSince = 0;
    if (session.idle && sessionTrivial(bus, path, "IdleSinceHint", 't', idleSince))
    {
        session.idleSinceUsec = idleSince;
    }
    std::uint32_t leader = 0;
    if (sessionTrivial(bus, path, "Leader", 'u', leader))
    {
        session.leaderPid = static_cast<std::int32_t>(leader);
    }
    return session;
}

} // namespace

LoginSessionsRead readLoginSessions(std::string_view currentSessionId)
{
    LoginSessionsRead read;
    Bus bus;
    if (const int result = Detail::openSystemBus(bus); result < 0)
    {
        read.error = std::string("The system bus couldn't be reached: ") + BusError().text(result);
        return read;
    }
    BusError error;
    Message reply;
    if (const int result = sd_bus_call_method(bus.get(), LOGIN1, LOGIN1_PATH, LOGIN1_MANAGER, "ListSessions", error.get(), reply.out(), "");
        result < 0)
    {
        read.error = error.is("org.freedesktop.DBus.Error.ServiceUnknown") || error.is("org.freedesktop.DBus.Error.NameHasNoOwner")
                       ? std::string("systemd-logind isn't running")
                       : "systemd-logind couldn't be asked: " + error.text(result);
        return read;
    }
    // a(susso): session ID, UID, user name, seat ID, session object.
    if (sd_bus_message_enter_container(reply.get(), 'a', "(susso)") < 0)
    {
        read.error = "systemd-logind's session list couldn't be read";
        return read;
    }
    std::vector<std::pair<UserSession, std::string>> listed;
    for (;;)
    {
        const char* id = nullptr;
        std::uint32_t uid = 0;
        const char* user = nullptr;
        const char* seat = nullptr;
        const char* path = nullptr;
        const int entry = sd_bus_message_read(reply.get(), "(susso)", &id, &uid, &user, &seat, &path);
        if (entry <= 0)
        {
            if (entry < 0)
            {
                read.error = "systemd-logind's session list couldn't be read";
                return read;
            }
            break;
        }
        UserSession session;
        session.id = id != nullptr ? id : "";
        session.uid = uid;
        session.user = user != nullptr ? user : "";
        session.seat = seat != nullptr ? seat : "";
        session.current = !currentSessionId.empty() && session.id == currentSessionId;
        listed.emplace_back(std::move(session), path != nullptr ? path : "");
    }
    read.sessions.reserve(listed.size());
    // No structured bindings here: CodeQL's extractor lost the call in one and reported readSession() and its
    // helpers as unused (cpp/unused-static-function).
    for (auto& entry : listed)
    {
        read.sessions.push_back(readSession(bus.get(), std::move(entry.first), entry.second));
    }
    read.ok = true;
    return read;
}

#else

LoginSessionsRead readLoginSessions(std::string_view /*currentSessionId*/)
{
    return {.ok = false, .error = "This build has no systemd support (it was built without libsystemd)", .sessions = {}};
}

#endif

} // namespace Platform::SystemdBus
