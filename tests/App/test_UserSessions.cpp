/// @file test_UserSessions.cpp
/// @brief The signed-in sessions behind Processes > Group: User (#3076, #3377): the model's order, Lock,
/// Disconnect and Sign out (the confirm, the worker, Authenticate and retry), and the sessions' feed, read only
/// while the Processes table is shown grouped by User. The session rows themselves and their text are
/// UserGrouping's (test_UserGrouping.cpp) and the Processes table's (test_ProcessesPanelRender.cpp).

#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/SessionActionView.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "Domain/SamplingConfig.h"
#include "Domain/UsersModel.h"
#include "Mocks/FakeUserSessions.h"
#include "Platform/BrokeredUserSessions.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/IUserSessions.h"
#include "Support/RestartOffered.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

using Platform::SessionAction;
using Platform::UserSession;

[[nodiscard]] UserSession session(std::string id, std::string user, std::uint32_t uid, std::string state = "active")
{
    UserSession s;
    s.id = std::move(id);
    s.user = std::move(user);
    s.uid = uid;
    s.state = std::move(state);
    s.sessionClass = "user";
    return s;
}

TEST(SessionMenuTest, HasItemsWhenThePlatformCanDoSomething)
{
    EXPECT_FALSE(SessionMenu{}.any());
    EXPECT_TRUE((SessionMenu{.lock = false, .disconnect = true, .signOut = false}.any()));
}

/// Sessions a test controls (tests/Mocks/FakeUserSessions.h), shared with the Processes table's Group: User tests.
using FakeSessions = TestMocks::FakeUserSessions;

TEST(UsersModelTest, PublishesTheSessionsByUserThenIdAndAFailedReadsReason)
{
    auto fake = std::make_shared<FakeSessions>(
        std::vector<UserSession>{session("7", "bob", 1001), session("3", "Alice", 1000), session("2", "alice", 1000)});
    Domain::UsersModel model(fake);
    EXPECT_TRUE(model.hasSessions());
    EXPECT_EQ(model.version(), 0U);
    model.sample();
    const auto publication = model.publication();
    ASSERT_EQ(publication->sessions.size(), 3U);
    EXPECT_EQ(publication->version, 1U);
    EXPECT_EQ(publication->status, Platform::UserSessionsStatus::Ok);
    EXPECT_EQ(publication->sessions[0].id, "2");
    EXPECT_EQ(publication->sessions[1].id, "3");
    EXPECT_EQ(publication->sessions[2].user, "bob");

    fake->m_Failure = "systemd-logind isn't running";
    model.sample();
    EXPECT_EQ(model.publication()->status, Platform::UserSessionsStatus::Failed);
    EXPECT_EQ(model.publication()->detail, "systemd-logind isn't running");
    EXPECT_THROW(Domain::UsersModel(nullptr), std::invalid_argument);
}

TEST(SessionActionTest, LockRunsAtOnceAndSignOutAsksFirstNamingTheSession)
{
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    SessionActionControl control(fake);
    UserSession desktop = session("2", "alice", 1000);
    desktop.seat = "seat0";

    control.lock(desktop);
    EXPECT_TRUE(control.busy() || !control.result().empty());
    control.update(0.0F, true);
    EXPECT_TRUE(control.result().ok);
    EXPECT_EQ(control.result().text, "Locked alice's session");
    EXPECT_TRUE(control.takeDone());
    EXPECT_FALSE(control.takeDone());

    control.requestSignOut(desktop);
    ASSERT_TRUE(control.pending().has_value());
    EXPECT_TRUE(control.showConfirm());
    EXPECT_TRUE(control.question().starts_with("Sign out alice from seat0?")) << control.question();
    EXPECT_TRUE(control.question().contains("unsaved work is lost"));
    EXPECT_FALSE(control.question().contains("TaskSmack closes too"));
    control.cancel();
    EXPECT_FALSE(control.pending().has_value());
    control.confirm(); // nothing pending
    control.update(0.0F, true);

    desktop.current = true;
    control.requestSignOut(desktop);
    EXPECT_TRUE(control.question().contains("TaskSmack closes too")) << control.question();
    control.confirm();
    control.update(0.0F, true);
    EXPECT_EQ(control.result().text, "Signed out alice's session");
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"lock 2", "signout 2"}));

    // The line goes after a while.
    control.update(SessionActionControl::RESULT_SECONDS + 1.0F);
    EXPECT_TRUE(control.result().empty());
}

TEST(SessionActionTest, AnotherUsersSessionOffersAuthenticateAndRetryOnce)
{
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    SessionActionControl control(fake);
    control.requestSignOut(session("5", "bob", 1001));
    control.confirm();
    control.update(0.0F, true);
    EXPECT_FALSE(control.result().ok);
    EXPECT_EQ(control.result().text, "Couldn't sign out bob's session: Requires administrator");
    ASSERT_TRUE(control.offersAuthenticateAndRetry());
    control.retryAuthorized();
    EXPECT_EQ(control.pendingText(), "Waiting for authorization to sign out bob's session...");
    control.update(0.0F, true);
    EXPECT_TRUE(control.result().ok);
    EXPECT_FALSE(control.offersAuthenticateAndRetry());
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"signout 5", "signout 5 interactive"}));

    control.retryAuthorized(); // nothing to retry
    SessionActionControl none(nullptr);
    none.lock(session("2", "alice", 1000));
    EXPECT_FALSE(none.busy());
}

// #3669: with no helper, another user's session is refused with nothing to retry it (BrokeredUserSessions says how to
// get the rights), and the line offers Restart as administrator... where TaskSmack can restart elevated.
TEST(SessionActionTest, WithNoHelperARefusalOffersRestartAsAdministrator)
{
    const auto noHelper = std::make_shared<Platform::BrokeredUserSessions>(std::make_unique<FakeSessions>(std::vector<UserSession>{}),
                                                                           std::shared_ptr<Platform::IPrivilegedBroker>{});
    SessionActionControl control(noHelper);
    control.requestSignOut(session("5", "bob", 1001));
    control.confirm();
    control.update(0.0F, true);
    EXPECT_FALSE(control.offersAuthenticateAndRetry());
    EXPECT_FALSE(control.offersRestartAsAdministrator()) << "elevated, Linux, or a synthetic run: never offered";

    const TestSupport::RestartOffered offered;
    control.requestSignOut(session("5", "bob", 1001));
    control.confirm();
    control.update(0.0F, true);
    EXPECT_FALSE(control.result().ok);
    EXPECT_FALSE(control.offersAuthenticateAndRetry());
    ASSERT_TRUE(control.offersRestartAsAdministrator());
    control.update(SessionActionControl::RESULT_SECONDS + 1.0F); // as long as a retry's: time to decide
    EXPECT_TRUE(control.offersRestartAsAdministrator());
    control.update(Detail::AUTHORIZE_RESULT_SECONDS);
    EXPECT_FALSE(control.offersRestartAsAdministrator());
    EXPECT_TRUE(offered.raised().empty()) << "nothing raised until it is pressed";

    // A helper's Authenticate and retry instead, and nothing for a session that went.
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    SessionActionControl withHelper(fake);
    withHelper.requestSignOut(session("5", "bob", 1001));
    withHelper.confirm();
    withHelper.update(0.0F, true);
    EXPECT_TRUE(withHelper.offersAuthenticateAndRetry());
    EXPECT_FALSE(withHelper.offersRestartAsAdministrator());
}

// #3076 (Windows): Disconnect asks first, as Sign out does. Another user's session is told its programs keep
// running; TaskSmack's own, that this screen locks. Refused for want of an administrator, it offers the retry.
TEST(SessionActionTest, DisconnectAsksFirstAndSaysWhatHappensToTheSession)
{
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{}, true, true);
    SessionActionControl control(fake);
    UserSession console = session("1", "alice", 0);
    console.type = "console";
    console.current = true;

    control.requestDisconnect(console);
    ASSERT_TRUE(control.pending().has_value());
    EXPECT_EQ(control.pendingAction(), SessionAction::Disconnect);
    EXPECT_TRUE(control.showConfirm());
    EXPECT_TRUE(control.question().starts_with("Disconnect alice from Console?")) << control.question();
    EXPECT_TRUE(control.question().contains("this screen locks")) << control.question();
    EXPECT_FALSE(control.question().contains("unsaved work")) << "a Disconnect keeps every program";
    control.confirm();
    control.update(0.0F, true);
    EXPECT_TRUE(control.result().ok);
    EXPECT_EQ(control.result().text, "Disconnected alice's session");

    control.requestDisconnect(session("5", "bob", 0));
    EXPECT_TRUE(control.question().contains("Their programs keep running")) << control.question();
    EXPECT_FALSE(control.question().contains("this screen locks"));
    control.confirm();
    control.update(0.0F, true);
    EXPECT_EQ(control.result().text, "Couldn't disconnect bob's session: Requires administrator");
    ASSERT_TRUE(control.offersAuthenticateAndRetry());
    control.retryAuthorized();
    EXPECT_EQ(control.pendingText(), "Waiting for authorization to disconnect bob's session...");
    control.update(0.0F, true);
    EXPECT_EQ(control.result().text, "Disconnected bob's session");
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"disconnect 1", "disconnect 5", "disconnect 5 interactive"}));

    // Asking for one while another is pending replaces it; a Sign out asks as before.
    control.requestDisconnect(session("5", "bob", 0));
    control.requestSignOut(session("5", "bob", 0));
    EXPECT_EQ(control.pendingAction(), SessionAction::SignOut);
    EXPECT_TRUE(control.question().contains("unsaved work is lost"));
    control.cancel();

    // Inside Remote Desktop, disconnecting TaskSmack's own session closes the connection; it locks no screen.
    UserSession remote = session("3", "alice", 0);
    remote.current = true;
    remote.remote = true;
    remote.type = "rdp";
    remote.remoteHost = "LAPTOP";
    const std::string question = UserGrouping::disconnectQuestion(remote);
    EXPECT_TRUE(question.contains("the Remote Desktop connection closes")) << question;
    EXPECT_FALSE(question.contains("this screen locks")) << question;

    EXPECT_STREQ(sessionActionWords(SessionAction::Lock).done, "Locked");
    EXPECT_STREQ(sessionActionWords(SessionAction::Disconnect).title, "Disconnect");
    EXPECT_STREQ(sessionActionWords(SessionAction::SignOut).title, "Sign out");
}

/// Waits up to five seconds for @p done.
template<typename Done> [[nodiscard]] bool waitFor(Done done)
{
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < giveUp)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

// #3377: the feed reads the sessions only while sessionsDemand() says a view shows them, at the Settings
// interval; hiding them closes the gate without joining the thread, and stop() joins it and lets go.
TEST(UserSessionsFeedTest, ReadsOnlyOnDemandAndStopsCleanly)
{
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{session("2", "alice", 1000)});
    UserSessionsFeed feed([fake] { return fake; });
    EXPECT_EQ(feed.model(), nullptr);
    EXPECT_EQ(feed.actions(), nullptr);
    EXPECT_FALSE(feed.canAct(SessionAction::SignOut));
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User); // before start(): nothing to read with yet
    EXPECT_FALSE(feed.demanded());
    feed.setProcessesView(false, ProcessGrouping::GroupBy::User);

    feed.start();
    ASSERT_NE(feed.model(), nullptr);
    ASSERT_NE(feed.actions(), nullptr);
    EXPECT_TRUE(feed.canAct(SessionAction::SignOut));
    const auto interval = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    feed.setInterval(interval);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fake->m_Reads.load(), 0) << "read with no view shown";

    // The Processes tab grouped any other way reads none; grouped by User, its groups show the sessions, so it
    // reads them while it is shown (#3377).
    feed.setProcessesView(true, ProcessGrouping::GroupBy::Unit);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fake->m_Reads.load(), 0) << "read for Group: Unit";
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    EXPECT_TRUE(feed.demanded());
    ASSERT_TRUE(waitFor([&feed] { return feed.publishedVersion() >= 1; })) << "not read for Group: User";
    feed.setProcessesView(false, ProcessGrouping::GroupBy::User);
    EXPECT_FALSE(feed.demanded()) << "the Processes tab hidden";

    // Shown again, grouped by User: read again at the interval.
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    EXPECT_TRUE(feed.demanded());
    const std::uint64_t shownAt = feed.publishedVersion();
    ASSERT_TRUE(waitFor([&feed, shownAt] { return feed.publishedVersion() >= shownAt + 2; })) << "not read again at the interval";

    // Hidden: the gate closes at once; a read already running may still finish.
    feed.setProcessesView(false, ProcessGrouping::GroupBy::User);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(interval);
    const int readsHidden = fake->m_Reads.load();
    std::this_thread::sleep_for(interval * 4);
    EXPECT_EQ(fake->m_Reads.load(), readsHidden) << "read while hidden";

    // Shown again: the running sampler reads at once, and again when an action asks.
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    ASSERT_TRUE(waitFor([&] { return fake->m_Reads.load() > readsHidden; }));
    feed.setInterval(std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
    std::this_thread::sleep_for(interval);
    const int readsBefore = fake->m_Reads.load();
    feed.requestRefresh();
    EXPECT_TRUE(waitFor([&] { return fake->m_Reads.load() > readsBefore; })) << "requestRefresh() read nothing";

    feed.stop();
    EXPECT_EQ(feed.model(), nullptr);
    EXPECT_EQ(feed.actions(), nullptr);
    EXPECT_FALSE(feed.demanded());
    const int readsStopped = fake->m_Reads.load();
    std::this_thread::sleep_for(interval * 2);
    EXPECT_EQ(fake->m_Reads.load(), readsStopped) << "read after stop()";
    feed.requestRefresh();                                          // no sampler: nothing to ask
    feed.stop();                                                    // twice is fine
    EXPECT_EQ(fake.use_count(), 2) << "the feed kept the sessions"; // this test's and the factory's copy
}

// A view shown before start() is read from as soon as the feed starts, whichever comes first (#3469 review).
TEST(UserSessionsFeedTest, AViewShownBeforeStartIsReadOnceStarted)
{
    UserSessionsFeed feed([] { return std::make_shared<FakeSessions>(std::vector<UserSession>{session("2", "alice", 1000)}); });
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    EXPECT_FALSE(feed.demanded()) << "nothing to read with before start()";
    feed.start();
    EXPECT_TRUE(feed.demanded());
    EXPECT_TRUE(waitFor([&feed] { return feed.publishedVersion() >= 1; })) << "shown, then started, but never read";
    feed.stop();
}

// stop() then start() with the view still shown reads again: the shown view outlives the restart.
TEST(UserSessionsFeedTest, ARestartWithTheViewStillShownReadsAgain)
{
    UserSessionsFeed feed([] { return std::make_shared<FakeSessions>(std::vector<UserSession>{session("2", "alice", 1000)}); });
    feed.start();
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    ASSERT_TRUE(waitFor([&feed] { return feed.publishedVersion() >= 1; }));
    feed.stop();
    EXPECT_FALSE(feed.demanded());
    feed.start();
    EXPECT_TRUE(feed.demanded());
    EXPECT_TRUE(waitFor([&feed] { return feed.publishedVersion() >= 1; })) << "restarted, still shown, but never read";
    feed.stop();
}

// Sessions the platform can't list are never read, and a feed that was never started stops cleanly.
TEST(UserSessionsFeedTest, UnsupportedSessionsAreNeverRead)
{
    const auto unsupported = std::make_shared<FakeSessions>(std::vector<UserSession>{}, false);
    const auto makeSessions = [&unsupported]
    {
        return std::shared_ptr<Platform::IUserSessions>(unsupported);
    };
    UserSessionsFeed feed(makeSessions);
    feed.start();
    feed.setProcessesView(true, ProcessGrouping::GroupBy::User);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(unsupported->m_Reads.load(), 0);
    EXPECT_FALSE(feed.canAct(SessionAction::SignOut));
    feed.stop();

    const UserSessionsFeed neverStarted(makeSessions);
    EXPECT_EQ(neverStarted.publishedVersion(), 0U);
}

} // namespace
} // namespace App
