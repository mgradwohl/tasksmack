/// @file test_WindowsUserSessions.cpp
/// @brief The Users tab's Windows sessions (#3076): the WTS reader and Disconnect and Sign out through fake
/// WTS calls, the helper's answer to a SessionActionRequest, and a read of this machine's real sessions.
/// No test here disconnects or signs out a real session: every action goes to the fakes.

#include "Platform/BrokerProtocol.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/Windows/WindowsUserSessions.h"
#include "Platform/Windows/WindowsUserSessionsMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>
#include <vector>

// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

namespace Platform::Windows
{
namespace
{

using UserSessionsMath::WtsSessionFacts;
using UserSessionsMath::WtsState;

[[nodiscard]] WtsSessionFacts facts(std::uint32_t id, WtsState state, std::string user)
{
    WtsSessionFacts session;
    session.sessionId = id;
    session.state = static_cast<std::uint32_t>(state);
    session.user = std::move(user);
    session.winStation = id == 0 ? "Services" : "Console";
    return session;
}

/// WTS as a test sets it up: what it lists, what each action answers, and every action asked of it.
struct FakeWts
{
    std::vector<WtsSessionFacts> sessions{
        facts(0, WtsState::Disconnected, ""), facts(1, WtsState::Active, "alice"), facts(3, WtsState::Disconnected, "bob")};
    std::string listFailure;
    std::uint32_t answer = 0; ///< What disconnect and logoff return
    std::vector<std::string> calls;

    [[nodiscard]] WtsCalls makeCalls()
    {
        return WtsCalls{
            .enumerate = [this] -> std::expected<std::vector<WtsSessionFacts>, std::string>
            {
                if (!listFailure.empty())
                {
                    return std::unexpected(listFailure);
                }
                return sessions;
            },
            .currentSessionId = 1,
            .disconnect =
                [this](std::uint32_t session)
            {
                calls.push_back("disconnect " + std::to_string(session));
                return answer;
            },
            .logoff =
                [this](std::uint32_t session)
            {
                calls.push_back("logoff " + std::to_string(session));
                return answer;
            },
            .describe = [](std::uint32_t error) { return error == 7022 ? std::string("The session can't be logged off") : std::string{}; },
        };
    }
};

TEST(WindowsUserSessionsTest, ReadsTheSignedInSessionsAndSaysWhyAFailedListFailed)
{
    FakeWts wts;
    WindowsUserSessions sessions(wts.makeCalls());
    EXPECT_TRUE(sessions.hasSessions());
    const UserSessionsRead read = sessions.read();
    ASSERT_EQ(read.status, UserSessionsStatus::Ok);
    ASSERT_EQ(read.sessions.size(), 2U) << "session 0 is left out";
    EXPECT_EQ(read.sessions[0].user, "alice");
    EXPECT_TRUE(read.sessions[0].current);
    EXPECT_EQ(read.sessions[1].user, "bob");
    EXPECT_EQ(read.sessions[1].state, "disconnected");
    EXPECT_FALSE(read.sessions[1].current);

    wts.listFailure = "Windows couldn't list the sessions: RPC server unavailable";
    const UserSessionsRead failed = sessions.read();
    EXPECT_EQ(failed.status, UserSessionsStatus::Failed);
    EXPECT_EQ(failed.detail, "Windows couldn't list the sessions: RPC server unavailable");
    EXPECT_TRUE(failed.sessions.empty());
}

TEST(WindowsUserSessionsTest, OffersDisconnectAndSignOutButNotLock)
{
    FakeWts wts;
    const WindowsUserSessions sessions(wts.makeCalls());
    EXPECT_TRUE(sessions.canAct(SessionAction::Disconnect));
    EXPECT_TRUE(sessions.canAct(SessionAction::SignOut));
    EXPECT_FALSE(sessions.canAct(SessionAction::Lock));
}

TEST(WindowsUserSessionsTest, DisconnectAndSignOutReachWtsOnlyForALiveSignedInSession)
{
    FakeWts wts;
    WindowsUserSessions sessions(wts.makeCalls());
    EXPECT_TRUE(sessions.act("1", SessionAction::Disconnect, false, {}).ok);
    EXPECT_TRUE(sessions.act("3", SessionAction::SignOut, true, {}).ok);
    // Refused before WTS is asked: session 0, an ended session, a Lock, a disconnected session's Disconnect, no number.
    for (const auto& [id, action] : std::vector<std::pair<std::string, SessionAction>>{{"0", SessionAction::SignOut},
                                                                                       {"0", SessionAction::Disconnect},
                                                                                       {"9", SessionAction::SignOut},
                                                                                       {"1", SessionAction::Lock},
                                                                                       {"3", SessionAction::Disconnect},
                                                                                       {"c1", SessionAction::SignOut}})
    {
        const ServiceActionResult refused = sessions.act(id, action, false, {});
        EXPECT_FALSE(refused.ok) << id;
        EXPECT_FALSE(refused.needsAuthorization) << id;
    }
    EXPECT_EQ(wts.calls, (std::vector<std::string>{"disconnect 1", "logoff 3"}));

    wts.listFailure = "Windows couldn't list the sessions: x";
    const ServiceActionResult unlisted = sessions.act("1", SessionAction::SignOut, false, {});
    EXPECT_FALSE(unlisted.ok);
    EXPECT_EQ(unlisted.message, "Windows couldn't list the sessions: x");
    EXPECT_EQ(wts.calls.size(), 2U) << "nothing is sent without a fresh list";
}

TEST(WindowsUserSessionsTest, AnotherUsersSessionIsRefusedForAnAdministratorToFinish)
{
    FakeWts wts;
    wts.answer = ERROR_ACCESS_DENIED;
    WindowsUserSessions sessions(wts.makeCalls());
    const ServiceActionResult denied = sessions.act("3", SessionAction::SignOut, false, {});
    EXPECT_FALSE(denied.ok);
    EXPECT_TRUE(denied.needsAuthorization);
    EXPECT_EQ(denied.message, "Requires administrator");

    wts.answer = 7022;
    const ServiceActionResult other = sessions.act("3", SessionAction::SignOut, false, {});
    EXPECT_FALSE(other.needsAuthorization);
    EXPECT_EQ(other.message, "The session can't be logged off");

    WtsCalls undescribed = wts.makeCalls();
    undescribed.describe = nullptr;
    wts.answer = 7023;
    EXPECT_EQ(actOnSession(undescribed, "3", SessionAction::SignOut).message, "Windows error 7023");
}

TEST(WindowsUserSessionsTest, TheHelperChecksTheSessionAndItsRefusalIsntOfferedAgain)
{
    FakeWts wts;
    const WtsCalls calls = wts.makeCalls();
    EXPECT_TRUE(handleHelperSessionAction(Broker::SessionActionRequest{.action = SessionAction::SignOut, .sessionId = 3}, calls).ok);
    const ServiceActionResult zero =
        handleHelperSessionAction(Broker::SessionActionRequest{.action = SessionAction::SignOut, .sessionId = 0}, calls);
    EXPECT_FALSE(zero.ok);
    EXPECT_EQ(zero.message, "Session 0 runs Windows' services; no one is signed in to it");
    EXPECT_FALSE(handleHelperSessionAction(Broker::SessionActionRequest{.action = SessionAction::Disconnect, .sessionId = 42}, calls).ok);
    wts.answer = ERROR_ACCESS_DENIED;
    const ServiceActionResult denied =
        handleHelperSessionAction(Broker::SessionActionRequest{.action = SessionAction::Disconnect, .sessionId = 1}, calls);
    EXPECT_FALSE(denied.ok);
    EXPECT_FALSE(denied.needsAuthorization) << "the helper is the administrator: nothing further could finish it";
    EXPECT_EQ(wts.calls, (std::vector<std::string>{"logoff 3", "disconnect 1"}));
}

// The real WTS calls, read only: this machine's sessions are listed, and the one this process runs in is
// among them. Nothing is disconnected or signed out.
TEST(WindowsUserSessionsRealTest, ListsThisMachinesSessionsAndFindsItsOwn)
{
    const WtsCalls calls = realWtsCalls();
    DWORD own = 0;
    ASSERT_NE(ProcessIdToSessionId(GetCurrentProcessId(), &own), FALSE);
    EXPECT_EQ(calls.currentSessionId, own);
    const auto listed = calls.enumerate();
    ASSERT_TRUE(listed.has_value()) << listed.error();
    EXPECT_TRUE(std::ranges::contains(*listed, own, &WtsSessionFacts::sessionId)) << "session " << own << " isn't listed";
    EXPECT_TRUE(std::ranges::contains(*listed, UserSessionsMath::SERVICES_SESSION, &WtsSessionFacts::sessionId)) << "no session 0";

    WindowsUserSessions sessions;
    const UserSessionsRead read = sessions.read();
    ASSERT_EQ(read.status, UserSessionsStatus::Ok) << read.detail;
    if (own == UserSessionsMath::SERVICES_SESSION)
    {
        GTEST_SKIP() << "this process runs in session 0 (a service, as a CI runner may be): no session of its own to find";
    }
    const auto mine = std::ranges::find_if(read.sessions, [](const UserSession& session) { return session.current; });
    ASSERT_NE(mine, read.sessions.end()) << "this process's session isn't among the signed-in ones";
    EXPECT_EQ(mine->id, std::to_string(own));
    EXPECT_FALSE(mine->user.empty());
    EXPECT_EQ(mine->sessionClass, "user");
    EXPECT_FALSE(calls.describe(ERROR_ACCESS_DENIED).empty());
}

} // namespace
} // namespace Platform::Windows
