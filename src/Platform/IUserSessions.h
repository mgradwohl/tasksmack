#pragma once

// The machine's signed-in sessions, for the Users tab (#3076): who is signed in, where (seat, TTY, remote
// host), how (X11, Wayland, a text console), and whether the session is active or idle; and the two things
// the tab can do to one: Lock or Disconnect, and Sign out. Linux reads logind over the system bus
// (ListSessions, then each session's properties) and asks it to lock or terminate a session, polkit deciding
// who may (another user's session asks for an administrator). Windows reads the Terminal Services sessions
// (WTSEnumerateSessionsW, WTSQuerySessionInformationW) and offers Disconnect in Lock's place
// (WTSDisconnectSession) and Sign out (WTSLogoffSession); another user's session goes through the
// administrator helper (BrokeredUserSessions.h). Each platform's canAct() says which actions it has.
//
// Sampled on the Users tab's own worker, at the Settings interval, only while the tab is shown (#2731). The
// App composition root creates it (Platform::makeUserSessions()).

#include "Platform/IServiceActions.h"

#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// One signed-in session.
struct UserSession
{
    std::string id;           ///< logind's session ID ("2", "c1"); Windows' session ID in decimal ("1")
    std::uint32_t uid = 0;    ///< The user it belongs to (0 on Windows, which has no UIDs)
    std::string user;         ///< Their name, as the Processes table's User column has it
    std::string domain;       ///< Windows: the user's domain or computer name; empty on Linux
    std::string seat;         ///< "seat0"; empty for a session on no seat (ssh). Windows: the WinStation ("RDP-Tcp#0")
    std::string tty;          ///< "tty2", "pts/0"; empty when none
    std::string type;         ///< "x11", "wayland", "tty", "mir", "unspecified"; Windows: "console", "rdp"
    std::string sessionClass; ///< "user", "greeter", "lock-screen", "background", "manager"
    /// "active", "online" (signed in, not in front), "closing"; Windows adds "disconnected" and "connecting"
    std::string state;
    std::string service;             ///< The PAM service that opened it: "gdm-password", "sshd", "login"
    std::string remoteHost;          ///< Where a remote session comes from (Windows: the client's name); empty for a local one
    bool remote = false;             ///< Opened from another machine (ssh, xrdp, Remote Desktop)
    bool idle = false;               ///< logind's IdleHint; on Windows, no input for a minute or more
    std::uint64_t idleSinceUsec = 0; ///< When it went idle (CLOCK_REALTIME µs); 0 when not idle
    std::uint64_t logonUsec = 0;     ///< Windows: when its user signed in (CLOCK_REALTIME µs); 0 when not known
    std::int32_t leaderPid = 0;      ///< The process that opened it (the display manager's helper, sshd's child)
    bool current = false;            ///< The session TaskSmack itself runs in
};

/// What came of one read.
enum class UserSessionsStatus : std::uint8_t
{
    Ok,          ///< `sessions` holds every session.
    Unsupported, ///< This run can't list sessions (synthetic runs, a build without sd-bus).
    Failed,      ///< The session manager couldn't be asked (no system bus, logind not running, WTS refused).
};

struct UserSessionsRead
{
    UserSessionsStatus status = UserSessionsStatus::Unsupported;
    std::vector<UserSession> sessions;
    std::string detail; ///< For Failed, what failed; else empty.
};

/// What the tab can do to a session.
enum class SessionAction : std::uint8_t
{
    Lock,       ///< Locks its screen (logind Session.Lock). Linux only: Windows offers Disconnect in its place
    SignOut,    ///< Ends it and every process in it (logind TerminateSession, WTSLogoffSession)
    Disconnect, ///< Detaches it from its screen or client, its programs still running (WTSDisconnectSession). Windows only
};

/// Lists the signed-in sessions, and locks or signs one out.
class IUserSessions
{
  public:
    virtual ~IUserSessions() = default;

    IUserSessions() = default;
    IUserSessions(const IUserSessions&) = default;
    IUserSessions& operator=(const IUserSessions&) = default;
    IUserSessions(IUserSessions&&) = default;
    IUserSessions& operator=(IUserSessions&&) = default;

    /// Whether this run can list sessions. When false the Users tab says so.
    [[nodiscard]] virtual bool hasSessions() const = 0;

    /// Reads every session now. Synchronous: called from the tab's sampler thread, never the UI thread.
    [[nodiscard]] virtual UserSessionsRead read() = 0;

    /// Whether act() can do @p action at all here.
    [[nodiscard]] virtual bool canAct(SessionAction /*action*/) const
    {
        return false;
    }

    /// Does @p action to session @p id. Unless @p allowInteractive, nothing may ask for a password: a call
    /// that would need one is refused with needsAuthorization set, and the user's "Authenticate and retry"
    /// calls again allowing it. Blocks until the session manager answers (or the prompt is answered, or
    /// @p stopToken is requested), so it runs on a worker. The default refuses.
    [[nodiscard]] virtual ServiceActionResult
    act(std::string_view /*id*/, SessionAction /*action*/, bool /*allowInteractive*/, const std::stop_token& /*stopToken*/)
    {
        return ServiceActionResult::failed("Not supported on this platform");
    }
};

/// The sessions of a run that can't list them.
class UnsupportedUserSessions final : public IUserSessions
{
  public:
    [[nodiscard]] bool hasSessions() const override
    {
        return false;
    }
    [[nodiscard]] UserSessionsRead read() override
    {
        return {};
    }
};

} // namespace Platform
