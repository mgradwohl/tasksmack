/// @file test_Users.cpp
/// @brief The Users tab (#3076): the model's order, Lock and Sign out (the confirm, the worker, Authenticate
/// and retry), the sessions' feed, read only on demand (#3377), and the panel, sampled only once shown, its rows and its session menu. The
/// rows themselves and the session text are UserGrouping's (test_UserGrouping.cpp, #3377).

#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/SessionActionControl.h"
#include "App/Panels/SessionActionView.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/Panels/UsersPanel.h"
#include "App/Panels/UsersView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/UsersModel.h"
#include "Mocks/FakeUserSessions.h"
#include "Mocks/MockProbes.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"
#include "Platform/ProcessTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
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
    feed.setUsersTabShown(true); // before start(): nothing to read with yet
    EXPECT_FALSE(feed.demanded());
    feed.setUsersTabShown(false);

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

    feed.setUsersTabShown(true);
    EXPECT_TRUE(feed.demanded());
    const std::uint64_t shownAt = feed.publishedVersion();
    ASSERT_TRUE(waitFor([&feed, shownAt] { return feed.publishedVersion() >= shownAt + 2; })) << "not read again at the interval";

    // Hidden: the gate closes at once; a read already running may still finish.
    feed.setUsersTabShown(false);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(interval);
    const int readsHidden = fake->m_Reads.load();
    std::this_thread::sleep_for(interval * 4);
    EXPECT_EQ(fake->m_Reads.load(), readsHidden) << "read while hidden";

    // Shown again: the running sampler reads at once, and again when an action asks.
    feed.setUsersTabShown(true);
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
    feed.setUsersTabShown(true);
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
    feed.setUsersTabShown(true);
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
    feed.setUsersTabShown(true);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(unsupported->m_Reads.load(), 0);
    EXPECT_FALSE(feed.canAct(SessionAction::SignOut));
    feed.stop();

    const UserSessionsFeed neverStarted(makeSessions);
    EXPECT_EQ(neverStarted.publishedVersion(), 0U);
}

class UsersPanelRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0F, 720.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }
    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// The users table's first column's middle and top edge, from the last frame that drew it.
    static inline float s_ColumnX = -1.0F;
    static inline float s_TableTop = -1.0F;

    static std::string frame(Panel& panel)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        panel.renderContent();
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        if (const ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID("##UsersTable")); table != nullptr)
        {
            s_ColumnX = (table->Columns[0].MinX + table->Columns[0].MaxX) * 0.5F;
            s_TableTop = table->OuterRect.Min.y;
        }
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    static std::string frameUntil(Panel& panel, const std::string& needle)
    {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string text = frame(panel);
        while (!text.contains(needle) && std::chrono::steady_clock::now() < giveUp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            text = frame(panel);
        }
        return text;
    }

    static void show(Panel& panel, const char* tab)
    {
        Core::ActiveTabChangedEvent event(tab);
        panel.onEvent(event);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(UsersPanelRenderTest, SamplesOnlyOnceShownListsTheUsersAndTheirSessionsAndSignsOutFromTheMenu)
{
    UserSession desktop = session("2", "alice", 1000);
    desktop.seat = "seat0";
    desktop.tty = "tty2";
    desktop.type = "wayland";
    desktop.current = true;
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{desktop, session("5", "bob", 1001, "online")});
    UserSessionsFeed feed([fake] { return fake; });
    UsersPanel panel(feed, [](Core::Event&) {});
    panel.onAttach();
    EXPECT_TRUE(frame(panel).contains("Reading users..."));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fake->m_Reads.load(), 0) << "sampled with the tab hidden";

    show(panel, "Users");
    const std::string text = frameUntil(panel, "bob");
    EXPECT_TRUE(text.contains("alice  (you)")) << text;
    EXPECT_TRUE(text.contains("1 session")) << text;
    EXPECT_TRUE(text.contains("seat0 \xC2\xB7 tty2 \xC2\xB7 Wayland  (this session)")) << text;
    EXPECT_TRUE(text.contains("Signed in")) << text;
    EXPECT_GE(fake->m_Reads.load(), 1);

    // Right-click bob's session row: the menu's Sign out... opens the confirm.
    ASSERT_NE(panel.actions(), nullptr);
    ASSERT_GT(s_TableTop, 0.0F);
    // Rows: header, alice, her session, bob, his session.
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const float x = s_ColumnX;
    const float y = s_TableTop + (rowHeight * 4.5F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(x, y);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(frame(panel));
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the session menu didn't open";
    const ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
    ASSERT_NE(menu, nullptr);
    // The second item, Sign out...
    const float itemHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
    io.AddMousePosEvent(menu->Pos.x + (menu->Size.x * 0.5F), menu->Pos.y + ImGui::GetStyle().WindowPadding.y + (itemHeight * 1.5F));
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(frame(panel));
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    ASSERT_TRUE(panel.actions()->pending().has_value());
    EXPECT_EQ(panel.actions()->pending()->id, "5");
    const std::string confirm = frame(panel);
    EXPECT_TRUE(confirm.contains("Sign out bob")) << confirm;

    // Confirmed (as the modal's button does): refused for want of an administrator, then retried.
    panel.actions()->confirm();
    panel.actions()->update(0.0F, true);
    const std::string refused = frame(panel);
    EXPECT_TRUE(refused.contains("Couldn't sign out bob's session")) << refused;
    panel.actions()->retryAuthorized();
    panel.actions()->update(0.0F, true);
    EXPECT_TRUE(frame(panel).contains("Signed out bob's session"));
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"signout 5", "signout 5 interactive"}));
    panel.onDetach();
    EXPECT_TRUE(frame(panel).empty());
}

// #3076 (Windows): the session menu has Disconnect... and Sign out..., never Lock. Disconnect... opens its own
// confirm, and is greyed for a session that is already disconnected.
TEST_F(UsersPanelRenderTest, WindowsSessionMenusOfferDisconnectInLocksPlace)
{
    UserSession console = session("1", "alice", 0);
    console.type = "console";
    console.current = true;
    UserSession away = session("4", "bob", 0, "disconnected");
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{console, away}, true, true);
    UserSessionsFeed feed([fake] { return fake; });
    UsersPanel panel(feed, [](Core::Event&) {});
    panel.onAttach();
    show(panel, "Users");
    const std::string text = frameUntil(panel, "bob");
    EXPECT_TRUE(text.contains("Console  (this session)")) << text;
    EXPECT_TRUE(text.contains("Disconnected")) << text;

    // Rows: header, alice, her session, bob, his session.
    ASSERT_GT(s_TableTop, 0.0F);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    ImGuiIO& io = ImGui::GetIO();
    const auto openMenuOn = [&](float row)
    {
        io.AddMousePosEvent(s_ColumnX, s_TableTop + (rowHeight * row));
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        return frame(panel);
    };

    // Resting on her session shows its tooltip: who, and which session.
    io.AddMousePosEvent(s_ColumnX, s_TableTop + (rowHeight * 2.5F));
    const std::string hovered = frameUntil(panel, "Session 1");
    EXPECT_TRUE(hovered.contains("Session 1")) << hovered;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    static_cast<void>(frame(panel));

    const auto clickItem = [&](int item)
    {
        const ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
        const float itemHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
        io.AddMousePosEvent(menu->Pos.x + (menu->Size.x * 0.5F),
                            menu->Pos.y + ImGui::GetStyle().WindowPadding.y + (itemHeight * (static_cast<float>(item) + 0.5F)));
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(frame(panel));
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    };

    // bob's disconnected session: its Disconnect... is greyed, so a click on it does nothing.
    const std::string bobsMenu = openMenuOn(4.5F);
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the session menu didn't open";
    EXPECT_TRUE(bobsMenu.contains("Disconnect...")) << bobsMenu;
    EXPECT_TRUE(bobsMenu.contains("Sign out...")) << bobsMenu;
    EXPECT_FALSE(bobsMenu.contains("Lock")) << bobsMenu;
    clickItem(0);
    EXPECT_FALSE(panel.actions()->pending().has_value()) << "an already disconnected session";
    // A click away closes the menu.
    io.AddMousePosEvent(ImGui::GetIO().DisplaySize.x - 4.0F, ImGui::GetIO().DisplaySize.y - 4.0F);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(frame(panel));

    // Her own session: Disconnect... opens its confirm, which says this screen locks.
    static_cast<void>(openMenuOn(2.5F));
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the session menu didn't open";
    clickItem(0);
    ASSERT_TRUE(panel.actions()->pending().has_value());
    EXPECT_EQ(panel.actions()->pending()->id, "1");
    EXPECT_EQ(panel.actions()->pendingAction(), SessionAction::Disconnect);
    const std::string confirm = frame(panel);
    EXPECT_TRUE(confirm.contains("Disconnect alice from Console?")) << confirm;
    EXPECT_TRUE(confirm.contains("this screen locks")) << confirm;
    panel.actions()->cancel();
    static_cast<void>(frame(panel));

    // Sign out... (the second item) asks as on Linux.
    static_cast<void>(openMenuOn(4.5F));
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the session menu didn't open";
    clickItem(1);
    ASSERT_TRUE(panel.actions()->pending().has_value());
    EXPECT_EQ(panel.actions()->pendingAction(), SessionAction::SignOut);
    EXPECT_TRUE(frame(panel).contains("Sign out bob")) << "the Sign out confirm";
    panel.actions()->confirm();
    panel.actions()->update(0.0F, true);
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"signout 4"}));
    panel.onDetach();
}

TEST_F(UsersPanelRenderTest, UnsupportedFailedAndEmptyReadsSaySo)
{
    auto unsupported = std::make_shared<FakeSessions>(std::vector<UserSession>{}, false);
    UserSessionsFeed noneFeed([unsupported] { return unsupported; });
    UsersPanel none(noneFeed, [](Core::Event&) {});
    none.onAttach();
    show(none, "Users");
    EXPECT_TRUE(frame(none).contains("Users aren't available"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(unsupported->m_Reads.load(), 0);
    none.onDetach();

    auto failing = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    failing->m_Failure = "systemd-logind isn't running";
    UserSessionsFeed failedFeed([failing] { return failing; });
    UsersPanel failed(failedFeed, [](Core::Event&) {});
    failed.onAttach();
    show(failed, "Users");
    const std::string text = frameUntil(failed, "couldn't be read");
    EXPECT_TRUE(text.contains("systemd-logind isn't running")) << text;
    Core::RefreshRateChangedEvent faster(250);
    failed.onEvent(faster);
    failed.onDetach();

    auto empty = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    UserSessionsFeed nobodyFeed([empty] { return empty; });
    UsersPanel nobody(nobodyFeed, [](Core::Event&) {});
    nobody.onAttach();
    show(nobody, "Users");
    EXPECT_TRUE(frameUntil(nobody, "No one is signed in").contains("No one is signed in"));
    show(nobody, "Processes");
    show(nobody, "Users"); // shown again: the running sampler reads at once
    nobody.onDetach();
}

// #3221: with the process model attached, each user's processes are listed under their sessions. A
// double-click on one selects it and opens its Details; the session menu's Lock locks at once, and its
// result line asks the sampler for the session's new state.
TEST_F(UsersPanelRenderTest, AUsersProcessesAreListedAndADoubleClickOpensTheirDetails)
{
    UserSession desktop = session("2", "alice", 1000);
    desktop.current = true;
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{desktop});
    std::int32_t selectedPid = 0;
    int detailsShown = 0;
    UserSessionsFeed feed([fake] { return fake; });
    UsersPanel panel(feed,
                     [&selectedPid, &detailsShown](Core::Event& event)
                     {
                         if (event.getEventType() == Core::ProcessSelectedEvent::getStaticType())
                         {
                             selectedPid = static_cast<Core::ProcessSelectedEvent&>(event).getPid();
                         }
                         else if (event.getEventType() == Core::ShowProcessDetailsEvent::getStaticType())
                         {
                             ++detailsShown;
                         }
                     });
    auto probe = std::make_unique<TestMocks::MockProcessProbe>();
    Platform::ProcessCounters server = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
    server.user = "alice";
    probe->withProcess(server);
    const auto processes = std::make_shared<Domain::ProcessModel>(std::move(probe));
    processes->refresh();
    panel.setProcessModel(processes);
    panel.onAttach();
    show(panel, "Users");
    const std::string text = frameUntil(panel, "server  (100)");
    ASSERT_TRUE(text.contains("server  (100)")) << text;

    // Rows: header, alice, her session, her process.
    ASSERT_GT(s_TableTop, 0.0F);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(s_ColumnX, s_TableTop + (rowHeight * 3.5F));
    static_cast<void>(frame(panel));
    for (int click = 0; click < 2; ++click)
    {
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(frame(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(frame(panel));
    }
    EXPECT_EQ(selectedPid, 100);
    EXPECT_EQ(detailsShown, 1) << "the second click of the double-click opens Details";

    // Right-click her session: the menu's first item, Lock, locks it without asking.
    io.AddMousePosEvent(s_ColumnX, s_TableTop + (rowHeight * 2.5F));
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
    static_cast<void>(frame(panel));
    ASSERT_GT(GImGui->OpenPopupStack.Size, 0) << "the session menu didn't open";
    const ImGuiWindow* menu = GImGui->OpenPopupStack.back().Window;
    ASSERT_NE(menu, nullptr);
    const float itemHeight = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y;
    io.AddMousePosEvent(menu->Pos.x + (menu->Size.x * 0.5F), menu->Pos.y + ImGui::GetStyle().WindowPadding.y + (itemHeight * 0.5F));
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(frame(panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(frame(panel));
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    const int readsBefore = fake->m_Reads.load();
    EXPECT_TRUE(frameUntil(panel, "Locked").contains("Locked"));
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"lock 2"}));
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake->m_Reads.load() == readsBefore && std::chrono::steady_clock::now() < giveUp)
    {
        static_cast<void>(frame(panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(fake->m_Reads.load(), readsBefore) << "the done action asks for a fresh read";
    panel.onDetach();
}

} // namespace
} // namespace App
