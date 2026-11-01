#pragma once

// Pure mappings behind WindowsUserSessions (#3076): one Terminal Services session's facts (what
// WTSQuerySessionInformationW's WTSSessionInfo, WTSClientName and WTSClientProtocolType give) as the
// Users tab's UserSession, the WTS states as the tab's state words, and the checks every Disconnect and
// Sign out passes before it is sent, in TaskSmack and in the administrator helper alike. No Windows
// header, so the tests build on every platform (tests/Platform/WindowsMath/).

#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Windows/WindowsConversions.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace Platform::Windows::UserSessionsMath
{

/// WTS_CONNECTSTATE_CLASS's values, in its order.
enum class WtsState : std::uint8_t
{
    Active,       ///< WTSActive: signed in and in front (the console's user, a connected Remote Desktop)
    Connected,    ///< WTSConnected: connected to a client, nobody signed in yet
    ConnectQuery, ///< WTSConnectQuery: connecting
    Shadow,       ///< WTSShadow: shadowing another session
    Disconnected, ///< WTSDisconnected: signed in, its screen detached; its programs still run
    Idle,         ///< WTSIdle: waiting for a client
    Listen,       ///< WTSListen: a listener (RDP-Tcp), never a person's session
    Reset,        ///< WTSReset: being reset
    Down,         ///< WTSDown: down with an error
    Init,         ///< WTSInit: initializing
};

/// WTSClientProtocolType's values.
inline constexpr std::uint16_t PROTOCOL_CONSOLE = 0;
inline constexpr std::uint16_t PROTOCOL_ICA = 1;
inline constexpr std::uint16_t PROTOCOL_RDP = 2;

/// Session 0 runs the services and no one signs in to it: never acted on.
inline constexpr std::uint32_t SERVICES_SESSION = 0;

/// A session with no input for this long reads as idle (WTSINFOW's LastInputTime against its CurrentTime).
inline constexpr std::uint64_t IDLE_AFTER_TICKS = 60 * FILETIME_TICKS_PER_SECOND;

/// One session, as WTS describes it. Times are FILETIME ticks (100 ns since 1601), 0 when not given.
struct WtsSessionFacts
{
    std::uint32_t sessionId = 0;
    std::uint32_t state = 0; ///< A WTS_CONNECTSTATE_CLASS value (WtsState)
    std::string winStation;  ///< "Console", "RDP-Tcp#0", "Services"; empty for a disconnected session
    std::string user;        ///< WTSINFOW::UserName; empty when nobody is signed in
    std::string domain;      ///< WTSINFOW::Domain
    std::string clientName;  ///< WTSClientName: the Remote Desktop client's computer
    std::uint16_t protocol = PROTOCOL_CONSOLE;
    std::uint64_t logonTime = 0;
    std::uint64_t lastInputTime = 0;
    std::uint64_t currentTime = 0;
};

/// The Users tab's state word for WTS state @p state: "active" (sessionStatus()'s Active), "online"
/// (Signed in), "disconnected", "connecting", "closing", "listening", or "unknown" past WTSInit.
[[nodiscard]] constexpr std::string_view stateName(std::uint32_t state) noexcept
{
    if (state > static_cast<std::uint32_t>(WtsState::Init))
    {
        return "unknown";
    }
    switch (static_cast<WtsState>(state))
    {
    case WtsState::Active:
    case WtsState::Shadow:
        return "active";
    case WtsState::Connected:
    case WtsState::Idle:
        return "online";
    case WtsState::ConnectQuery:
    case WtsState::Init:
        return "connecting";
    case WtsState::Disconnected:
        return "disconnected";
    case WtsState::Reset:
    case WtsState::Down:
        return "closing";
    case WtsState::Listen:
        return "listening";
    }
    return "unknown";
}

/// "rdp" for a Remote Desktop (or ICA) session, "console" for the machine's own screen, else empty (a
/// disconnected session has no client to say).
[[nodiscard]] inline std::string sessionType(const WtsSessionFacts& facts)
{
    if (facts.protocol == PROTOCOL_RDP || facts.protocol == PROTOCOL_ICA)
    {
        return "rdp";
    }
    return facts.winStation == "Console" ? std::string("console") : std::string{};
}

/// FILETIME ticks as CLOCK_REALTIME µs; 0 for one at or before the Unix epoch.
[[nodiscard]] constexpr std::uint64_t fileTimeToUnixUsec(std::uint64_t fileTime) noexcept
{
    constexpr std::uint64_t TICKS_PER_USEC = 10;
    return fileTime > FILETIME_UNIX_EPOCH ? (fileTime - FILETIME_UNIX_EPOCH) / TICKS_PER_USEC : 0;
}

/// @p facts as the Users tab's session, or nullopt for one that isn't a person's: session 0 (the
/// services), a listener, or one nobody is signed in to. @p currentSessionId is TaskSmack's own.
[[nodiscard]] inline std::optional<UserSession> toUserSession(const WtsSessionFacts& facts, std::uint32_t currentSessionId)
{
    if (facts.sessionId == SERVICES_SESSION || facts.user.empty() || facts.state == static_cast<std::uint32_t>(WtsState::Listen))
    {
        return std::nullopt;
    }
    UserSession session;
    session.id = std::to_string(facts.sessionId);
    session.user = facts.user;
    session.domain = facts.domain;
    session.type = sessionType(facts);
    session.seat = session.type == "rdp" ? facts.winStation : std::string{};
    session.sessionClass = "user";
    session.state = std::string(stateName(facts.state));
    session.remote = session.type == "rdp";
    session.remoteHost = session.remote ? facts.clientName : std::string{};
    session.logonUsec = fileTimeToUnixUsec(facts.logonTime);
    const bool signedInHere = session.state == "active" || session.state == "online";
    if (signedInHere && facts.lastInputTime != 0 && facts.currentTime > facts.lastInputTime &&
        facts.currentTime - facts.lastInputTime >= IDLE_AFTER_TICKS)
    {
        session.idle = true;
        session.idleSinceUsec = fileTimeToUnixUsec(facts.lastInputTime);
    }
    session.current = facts.sessionId == currentSessionId;
    return session;
}

/// Session ID @p id ("1", "12"), or nullopt for anything that isn't one whole decimal number.
[[nodiscard]] inline std::optional<std::uint32_t> parseSessionId(std::string_view id) noexcept
{
    std::uint32_t value = 0;
    const auto [stop, error] = std::from_chars(id.data(), id.data() + id.size(), value);
    if (id.empty() || error != std::errc{} || stop != id.data() + id.size())
    {
        return std::nullopt;
    }
    return value;
}

/// Whether @p action may be sent to session @p id, given the sessions WTS lists now (@p sessions, read
/// just before): its numeric ID, or why not. Refused: anything but Disconnect and Sign out (Windows has
/// no Lock here), an ID that isn't a number, session 0, a session that has ended, one nobody is signed
/// in to, and a Disconnect of one already disconnected. TaskSmack checks this before its own call, and
/// the administrator helper again before it acts for TaskSmack, so a request names a live sign-in only.
[[nodiscard]] inline std::expected<std::uint32_t, std::string>
checkSessionAction(std::string_view id, SessionAction action, std::span<const WtsSessionFacts> sessions)
{
    if (action != SessionAction::Disconnect && action != SessionAction::SignOut)
    {
        return std::unexpected(std::string("Windows has no Lock here; Disconnect takes its place"));
    }
    const std::optional<std::uint32_t> parsed = parseSessionId(id);
    if (!parsed.has_value())
    {
        return std::unexpected(std::format("\"{}\" isn't a session", id));
    }
    const std::uint32_t sessionId = *parsed;
    if (sessionId == SERVICES_SESSION)
    {
        return std::unexpected(std::string("Session 0 runs Windows' services; no one is signed in to it"));
    }
    const auto found = std::ranges::find(sessions, sessionId, &WtsSessionFacts::sessionId);
    if (found == sessions.end())
    {
        return std::unexpected(std::format("Session {} has ended", sessionId));
    }
    if (found->user.empty() || found->state == static_cast<std::uint32_t>(WtsState::Listen))
    {
        return std::unexpected(std::format("No one is signed in to session {}", sessionId));
    }
    if (action == SessionAction::Disconnect && found->state == static_cast<std::uint32_t>(WtsState::Disconnected))
    {
        return std::unexpected(std::format("Session {} is already disconnected", sessionId));
    }
    return sessionId;
}

/// ERROR_ACCESS_DENIED, which WTSDisconnectSession and WTSLogoffSession give for another user's session
/// without an administrator's rights: the one refusal the administrator helper could finish.
inline constexpr std::uint32_t ERROR_ACCESS_DENIED_CODE = 5;

/// The result of a refused WTSDisconnectSession or WTSLogoffSession that failed with Win32 error @p error
/// (described by @p description): access denied is "Requires administrator", with needsAuthorization set.
[[nodiscard]] inline ServiceActionResult refusalOf(std::uint32_t error, std::string_view description)
{
    if (error == ERROR_ACCESS_DENIED_CODE)
    {
        ServiceActionResult refused = ServiceActionResult::failed("Requires administrator");
        refused.needsAuthorization = true;
        return refused;
    }
    return ServiceActionResult::failed(description.empty() ? std::format("Windows error {}", error) : std::string(description));
}

} // namespace Platform::Windows::UserSessionsMath
