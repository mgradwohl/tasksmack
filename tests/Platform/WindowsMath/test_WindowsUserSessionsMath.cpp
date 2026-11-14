/// @file test_WindowsUserSessionsMath.cpp
/// @brief WindowsUserSessionsMath.h (#3076): a WTS session's facts as a UserSession, the WTS states
/// as its words, and the checks a Disconnect or Sign out passes before it is sent (session 0, an ended
/// session, one nobody is signed in to). No Windows header, so these run on every platform.

#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Windows/WindowsConversions.h"
#include "Platform/Windows/WindowsUserSessionsMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Platform::Windows::UserSessionsMath
{
namespace
{

constexpr std::uint64_t SECOND = FILETIME_TICKS_PER_SECOND;
/// 2023-11-14 22:13:20 UTC, as FILETIME ticks.
constexpr std::uint64_t NOW = FILETIME_UNIX_EPOCH + (1'700'000'000ULL * SECOND);
/// When the test's users signed in: an hour before NOW.
constexpr std::uint64_t SIGNED_IN = NOW - (3600 * SECOND);

[[nodiscard]] WtsSessionFacts facts(std::uint32_t id, WtsState state, std::string user, std::string winStation = "Console")
{
    WtsSessionFacts session;
    session.sessionId = id;
    session.state = static_cast<std::uint32_t>(state);
    session.domain = user.empty() ? std::string{} : std::string("PC");
    session.user = std::move(user);
    session.winStation = std::move(winStation);
    session.currentTime = NOW;
    session.logonTime = session.user.empty() ? 0 : SIGNED_IN;
    return session;
}

/// The session the user confirmed: @p user of PC, signed in at SIGNED_IN, in session @p id.
[[nodiscard]] UserSession confirmed(std::string id, std::string user = "alice", std::string domain = "PC")
{
    UserSession session;
    session.id = std::move(id);
    session.user = std::move(user);
    session.domain = std::move(domain);
    session.logonUsec = fileTimeToUnixUsec(SIGNED_IN);
    return session;
}

TEST(WindowsUserSessionsMathTest, EveryWtsStateHasItsWord)
{
    struct Row
    {
        WtsState state;
        std::string_view word;
    };
    for (const Row& row : {Row{.state = WtsState::Active, .word = "active"},
                           Row{.state = WtsState::Connected, .word = "online"},
                           Row{.state = WtsState::ConnectQuery, .word = "connecting"},
                           Row{.state = WtsState::Shadow, .word = "active"},
                           Row{.state = WtsState::Disconnected, .word = "disconnected"},
                           Row{.state = WtsState::Idle, .word = "online"},
                           Row{.state = WtsState::Listen, .word = "listening"},
                           Row{.state = WtsState::Reset, .word = "closing"},
                           Row{.state = WtsState::Down, .word = "closing"},
                           Row{.state = WtsState::Init, .word = "connecting"}})
    {
        EXPECT_EQ(stateName(static_cast<std::uint32_t>(row.state)), row.word) << static_cast<int>(row.state);
    }
    EXPECT_EQ(stateName(10), "unknown") << "past WTSInit";
    EXPECT_EQ(stateName(0xFFFFFFFFU), "unknown");
}

TEST(WindowsUserSessionsMathTest, TheConsoleAndARemoteDesktopAreTheirTypes)
{
    const WtsSessionFacts console = facts(1, WtsState::Active, "alice");
    EXPECT_EQ(sessionType(console), "console");
    WtsSessionFacts rdp = facts(3, WtsState::Active, "bob", "RDP-Tcp#0");
    rdp.protocol = PROTOCOL_RDP;
    EXPECT_EQ(sessionType(rdp), "rdp");
    rdp.protocol = PROTOCOL_ICA;
    EXPECT_EQ(sessionType(rdp), "rdp");
    const WtsSessionFacts away = facts(4, WtsState::Disconnected, "carol", "");
    EXPECT_EQ(sessionType(away), "") << "a disconnected session has no client";
}

TEST(WindowsUserSessionsMathTest, ASignedInSessionIsTheTabsSessionAndTheOthersArent)
{
    WtsSessionFacts console = facts(1, WtsState::Active, "alice");
    console.logonTime = NOW - (3600 * SECOND);
    const std::optional<UserSession> mine = toUserSession(console, 1);
    ASSERT_TRUE(mine.has_value());
    EXPECT_EQ(mine->id, "1");
    EXPECT_EQ(mine->user, "alice");
    EXPECT_EQ(mine->domain, "PC");
    EXPECT_EQ(mine->type, "console");
    EXPECT_EQ(mine->seat, "") << "the console's WinStation name says nothing the type doesn't";
    EXPECT_EQ(mine->sessionClass, "user");
    EXPECT_EQ(mine->state, "active");
    EXPECT_FALSE(mine->remote);
    EXPECT_EQ(mine->logonUsec, (1'700'000'000ULL - 3600ULL) * 1'000'000ULL);
    EXPECT_FALSE(mine->idle) << "no last input given";
    EXPECT_TRUE(mine->current);

    WtsSessionFacts rdp = facts(3, WtsState::Active, "bob", "RDP-Tcp#0");
    rdp.protocol = PROTOCOL_RDP;
    rdp.clientName = "LAPTOP";
    rdp.lastInputTime = NOW - (300 * SECOND);
    const std::optional<UserSession> remote = toUserSession(rdp, 1);
    ASSERT_TRUE(remote.has_value());
    EXPECT_EQ(remote->seat, "RDP-Tcp#0");
    EXPECT_TRUE(remote->remote);
    EXPECT_EQ(remote->remoteHost, "LAPTOP");
    EXPECT_TRUE(remote->idle);
    EXPECT_EQ(remote->idleSinceUsec, (1'700'000'000ULL - 300ULL) * 1'000'000ULL);
    EXPECT_FALSE(remote->current);

    rdp.lastInputTime = NOW - (30 * SECOND);
    EXPECT_FALSE(toUserSession(rdp, 1)->idle) << "under a minute";
    rdp.lastInputTime = NOW + SECOND;
    EXPECT_FALSE(toUserSession(rdp, 1)->idle) << "a clock that went back";
    WtsSessionFacts away = facts(4, WtsState::Disconnected, "carol", "");
    away.lastInputTime = NOW - (3600 * SECOND);
    const std::optional<UserSession> disconnected = toUserSession(away, 1);
    ASSERT_TRUE(disconnected.has_value());
    EXPECT_EQ(disconnected->state, "disconnected");
    EXPECT_FALSE(disconnected->idle) << "a disconnected session says so, not idle";

    EXPECT_FALSE(toUserSession(facts(0, WtsState::Disconnected, "SYSTEM", "Services"), 1).has_value()) << "session 0";
    EXPECT_FALSE(toUserSession(facts(65536, WtsState::Listen, "", "RDP-Tcp"), 1).has_value()) << "a listener";
    EXPECT_FALSE(toUserSession(facts(2, WtsState::Connected, ""), 1).has_value()) << "nobody signed in yet";
    EXPECT_FALSE(toUserSession(facts(5, WtsState::Listen, "dave"), 1).has_value()) << "a listener with a name";

    EXPECT_EQ(fileTimeToUnixUsec(0), 0U);
    EXPECT_EQ(fileTimeToUnixUsec(FILETIME_UNIX_EPOCH), 0U);
    EXPECT_EQ(fileTimeToUnixUsec(FILETIME_UNIX_EPOCH + 10), 1U);
}

TEST(WindowsUserSessionsMathTest, OnlyAWholeDecimalNumberIsASessionId)
{
    EXPECT_EQ(parseSessionId("1"), 1U);
    EXPECT_EQ(parseSessionId("4294967295"), 4294967295U);
    for (const std::string_view bad : {"", "-1", "+1", " 1", "1 ", "1a", "0x1", "4294967296", "c1"})
    {
        EXPECT_FALSE(parseSessionId(bad).has_value()) << '"' << bad << '"';
    }
}

TEST(WindowsUserSessionsMathTest, AnActionIsSentOnlyToALiveSignedInSessionThatIsntSessionZero)
{
    const WtsSessionFacts away = facts(4, WtsState::Disconnected, "carol", "");
    const std::vector<WtsSessionFacts> sessions{facts(0, WtsState::Disconnected, "", "Services"),
                                                facts(1, WtsState::Active, "alice"),
                                                facts(2, WtsState::Connected, ""),
                                                away,
                                                facts(65536, WtsState::Listen, "", "RDP-Tcp")};
    struct Row
    {
        std::string_view id;
        SessionAction action;
        std::optional<std::uint32_t> sent; ///< nullopt: refused
        std::string_view refusal;
        std::string_view user = "alice"; ///< Whom the user confirmed
    };
    for (const Row& row : {
             Row{.id = "1", .action = SessionAction::SignOut, .sent = 1U, .refusal = {}},
             Row{.id = "1", .action = SessionAction::Disconnect, .sent = 1U, .refusal = {}},
             Row{.id = "4", .action = SessionAction::SignOut, .sent = 4U, .refusal = {}, .user = "carol"},
             Row{.id = "4",
                 .action = SessionAction::Disconnect,
                 .sent = std::nullopt,
                 .refusal = "Session 4 is already disconnected",
                 .user = "carol"},
             Row{.id = "1",
                 .action = SessionAction::Lock,
                 .sent = std::nullopt,
                 .refusal = "Windows has no Lock here; Disconnect takes its place"},
             Row{.id = "0",
                 .action = SessionAction::SignOut,
                 .sent = std::nullopt,
                 .refusal = "Session 0 runs Windows' services; no one is signed in to it"},
             Row{.id = "0",
                 .action = SessionAction::Disconnect,
                 .sent = std::nullopt,
                 .refusal = "Session 0 runs Windows' services; no one is signed in to it"},
             Row{.id = "9", .action = SessionAction::SignOut, .sent = std::nullopt, .refusal = "Session 9 has ended"},
             Row{.id = "2", .action = SessionAction::SignOut, .sent = std::nullopt, .refusal = "No one is signed in to session 2"},
             Row{.id = "65536",
                 .action = SessionAction::Disconnect,
                 .sent = std::nullopt,
                 .refusal = "No one is signed in to session 65536"},
             Row{.id = "c1", .action = SessionAction::SignOut, .sent = std::nullopt, .refusal = "\"c1\" isn't a session"},
             Row{.id = "", .action = SessionAction::SignOut, .sent = std::nullopt, .refusal = "\"\" isn't a session"},
         })
    {
        const auto checked = checkSessionAction(confirmed(std::string(row.id), std::string(row.user)), row.action, sessions);
        if (row.sent.has_value())
        {
            ASSERT_TRUE(checked.has_value()) << row.id << ": " << checked.error();
            EXPECT_EQ(*checked, *row.sent);
        }
        else
        {
            ASSERT_FALSE(checked.has_value()) << row.id;
            EXPECT_EQ(checked.error(), row.refusal) << row.id;
        }
    }
    EXPECT_FALSE(checkSessionAction(confirmed("1"), SessionAction::SignOut, {}).has_value()) << "nothing listed: it has ended";
}

// #3076: Windows reuses session IDs. If alice's session 1 ends while her Sign out is being confirmed (or its
// UAC prompt answered) and bob's sign-in gets ID 1, the action is refused: the ID no longer names the
// sign-in the user confirmed. So is one in another domain, a later sign-in of the same user, and a request
// that names no sign-in at all.
TEST(WindowsUserSessionsMathTest, AnIdReusedByAnotherSignInIsRefused)
{
    const std::vector<WtsSessionFacts> bobInOne{facts(1, WtsState::Active, "bob")};
    const auto reused = checkSessionAction(confirmed("1", "alice"), SessionAction::SignOut, bobInOne);
    ASSERT_FALSE(reused.has_value());
    EXPECT_EQ(reused.error(), "Session 1 is now another sign-in than the one confirmed (alice), so nothing was done");

    const std::vector<WtsSessionFacts> aliceInOne{facts(1, WtsState::Active, "alice")};
    EXPECT_FALSE(checkSessionAction(confirmed("1", "alice", "CORP"), SessionAction::Disconnect, aliceInOne).has_value())
        << "CORP\\alice isn't PC\\alice";
    std::vector<WtsSessionFacts> later = aliceInOne;
    later[0].logonTime = NOW;
    EXPECT_FALSE(checkSessionAction(confirmed("1"), SessionAction::SignOut, later).has_value()) << "alice signed in again since";
    EXPECT_TRUE(checkSessionAction(confirmed("1"), SessionAction::SignOut, aliceInOne).has_value()) << "the same sign-in";

    UserSession nobody = confirmed("1");
    nobody.user.clear();
    const auto anonymous = checkSessionAction(nobody, SessionAction::SignOut, aliceInOne);
    ASSERT_FALSE(anonymous.has_value());
    EXPECT_EQ(anonymous.error(), "Which sign-in session 1 was isn't known, so nothing was done");
    UserSession whenUnknown = confirmed("1");
    whenUnknown.logonUsec = 0;
    EXPECT_FALSE(checkSessionAction(whenUnknown, SessionAction::SignOut, aliceInOne).has_value()) << "no sign-in time";
}

// #3076: a session whose WTSSessionInfo couldn't be read (access denied to another user's session) is kept,
// as not readable, and is never taken for one nobody is signed in to; nothing is sent to it.
TEST(WindowsUserSessionsMathTest, ASessionWhoseDetailsCantBeReadIsKeptAndNotActedOn)
{
    WtsSessionFacts hidden = facts(3, WtsState::Disconnected, "", "");
    hidden.infoError = "Access is denied";
    const std::optional<UserSession> session = toUserSession(hidden, 1);
    ASSERT_TRUE(session.has_value());
    EXPECT_EQ(session.value_or(UserSession{}).id, "3");
    EXPECT_EQ(session.value_or(UserSession{}).unreadable, "Access is denied");
    EXPECT_EQ(session.value_or(UserSession{}).state, "disconnected");
    EXPECT_TRUE(session.value_or(UserSession{}).user.empty());
    EXPECT_EQ(accountKey(session.value_or(UserSession{})), "?3") << "a row of its own";

    const std::vector<WtsSessionFacts> listed{hidden};
    const auto refused = checkSessionAction(confirmed("3"), SessionAction::SignOut, listed);
    ASSERT_FALSE(refused.has_value());
    EXPECT_EQ(refused.error(), "Session 3's user couldn't be read: Access is denied");
    hidden.sessionId = 0;
    EXPECT_FALSE(toUserSession(hidden, 1).has_value()) << "session 0 is still left out";
}

TEST(WindowsUserSessionsMathTest, AccessDeniedIsTheOneRefusalAnAdministratorCouldFinish)
{
    const ServiceActionResult denied = refusalOf(ERROR_ACCESS_DENIED_CODE, "Access is denied");
    EXPECT_FALSE(denied.ok);
    EXPECT_TRUE(denied.needsAuthorization);
    EXPECT_EQ(denied.message, "Requires administrator");
    const ServiceActionResult other = refusalOf(7022, "The requested session cannot be logged off");
    EXPECT_FALSE(other.needsAuthorization);
    EXPECT_EQ(other.message, "The requested session cannot be logged off");
    EXPECT_EQ(refusalOf(7022, "").message, "Windows error 7022");
}

} // namespace
} // namespace Platform::Windows::UserSessionsMath
