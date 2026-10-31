/// @file test_Users.cpp
/// @brief The Users tab (#3076): a row per signed-in user with their sessions and their processes' totals,
/// the session text, the model's order, Lock and Sign out (the confirm, the worker, Authenticate and retry),
/// and the panel, sampled only once shown, its rows and its session menu.

#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UsersPanel.h"
#include "App/Panels/UsersView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
#include "Platform/IServiceActions.h"
#include "Platform/IUserSessions.h"

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

[[nodiscard]] Domain::ProcessSnapshot process(std::int32_t pid, std::string user, double cpu, std::uint64_t memory)
{
    Domain::ProcessSnapshot p;
    p.pid = pid;
    p.name = "proc" + std::to_string(pid);
    p.user = std::move(user);
    p.cpuPercent = cpu;
    p.memoryBytes = memory;
    p.ioReadBytesPerSec = 10.0;
    p.ioWriteBytesPerSec = 5.0;
    p.netSentBytesPerSec = 2.0;
    p.netReceivedBytesPerSec = 3.0;
    return p;
}

TEST(UserRowsTest, OneRowPerSignedInUserWithTheirSessionsAndProcessTotals)
{
    UserSession greeter = session("c1", "gdm", 120);
    greeter.sessionClass = "greeter";
    UserSession aliceDesktop = session("2", "alice", 1000);
    aliceDesktop.current = true;
    const std::vector<UserSession> sessions{aliceDesktop, session("7", "alice", 1000, "online"), session("5", "Bob", 1001), greeter};
    const std::vector<Domain::ProcessSnapshot> processes{
        process(10, "alice", 5.0, 100), process(11, "alice", 20.0, 300), process(12, "Bob", 1.0, 50), process(13, "root", 50.0, 9000)};

    const std::vector<UserRow> rows = buildUserRows(sessions, processes);
    ASSERT_EQ(rows.size(), 2U) << "the greeter and root (no sign-in) have no row";
    EXPECT_EQ(rows[0].user, "alice");
    EXPECT_EQ(rows[0].uid, 1000U);
    EXPECT_EQ(rows[0].sessions.size(), 2U);
    EXPECT_TRUE(rows[0].current);
    EXPECT_EQ(rows[0].totals.processes, 2U);
    EXPECT_DOUBLE_EQ(rows[0].totals.cpuPercent, 25.0);
    EXPECT_EQ(rows[0].totals.resident, 400U);
    EXPECT_DOUBLE_EQ(rows[0].totals.ioRead + rows[0].totals.ioWrite, 30.0);
    EXPECT_DOUBLE_EQ(rows[0].totals.netSent + rows[0].totals.netReceived, 10.0);
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{1, 0})) << "busiest first";
    EXPECT_EQ(rows[1].user, "Bob") << "by name, ignoring case";
    EXPECT_FALSE(rows[1].current);
    EXPECT_EQ(rows[1].processes, (std::vector<std::size_t>{2}));

    EXPECT_TRUE(isSignInSession(session("1", "x", 1)));
    UserSession unclassed = session("1", "x", 1);
    unclassed.sessionClass.clear();
    EXPECT_TRUE(isSignInSession(unclassed));
    unclassed.sessionClass = "manager";
    EXPECT_FALSE(isSignInSession(unclassed));
}

TEST(UserRowsTest, TheSessionTextSaysWhereHowAndWhatItIsDoing)
{
    UserSession desktop = session("2", "alice", 1000);
    desktop.seat = "seat0";
    desktop.tty = "tty2";
    desktop.type = "wayland";
    EXPECT_EQ(sessionPlace(desktop), "seat0 \xC2\xB7 tty2 \xC2\xB7 Wayland");
    UserSession ssh = session("5", "bob", 1001);
    ssh.tty = "pts/0";
    ssh.type = "tty";
    ssh.remote = true;
    ssh.remoteHost = "10.0.0.9";
    EXPECT_EQ(sessionPlace(ssh), "pts/0 \xC2\xB7 Console \xC2\xB7 from 10.0.0.9");
    ssh.remoteHost.clear();
    ssh.type = "x11";
    EXPECT_EQ(sessionPlace(ssh), "pts/0 \xC2\xB7 X11 \xC2\xB7 remote");
    UserSession bare = session("9", "carol", 1002);
    bare.type = "unspecified";
    EXPECT_EQ(sessionPlace(bare), "Session 9");
    bare.type = "mir";
    EXPECT_EQ(sessionPlace(bare), "Mir");
    bare.type = "custom";
    EXPECT_EQ(sessionPlace(bare), "custom");

    constexpr std::uint64_t MINUTE = 60'000'000;
    const std::uint64_t now = 1'000 * MINUTE;
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "active"), now), "Active");
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "online"), now), "Signed in");
    EXPECT_EQ(sessionStatus(session("1", "a", 1, "closing"), now), "Closing");
    UserSession idle = session("1", "a", 1);
    idle.idle = true;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "no idle-since";
    idle.idleSinceUsec = now - (12 * MINUTE);
    EXPECT_EQ(sessionStatus(idle, now), "Idle for 12 min");
    idle.idleSinceUsec = now - (125 * MINUTE);
    EXPECT_EQ(sessionStatus(idle, now), "Idle for 2 h 5 min");
    idle.idleSinceUsec = now - 1000;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "under a minute";
    idle.idleSinceUsec = now + MINUTE;
    EXPECT_EQ(sessionStatus(idle, now), "Idle") << "a clock that went back";
}

/// Sessions a test controls: what read() lists and what act() answers, with each call recorded.
class FakeSessions final : public Platform::IUserSessions
{
  public:
    explicit FakeSessions(std::vector<UserSession> sessions, bool has = true) : m_Sessions(std::move(sessions)), m_Has(has)
    {}
    [[nodiscard]] bool hasSessions() const override
    {
        return m_Has;
    }
    [[nodiscard]] Platform::UserSessionsRead read() override
    {
        ++m_Reads;
        if (!m_Failure.empty())
        {
            return {.status = Platform::UserSessionsStatus::Failed, .sessions = {}, .detail = m_Failure};
        }
        return {.status = Platform::UserSessionsStatus::Ok, .sessions = m_Sessions, .detail = {}};
    }
    [[nodiscard]] bool canAct(SessionAction /*action*/) const override
    {
        return m_Has;
    }
    [[nodiscard]] Platform::ServiceActionResult
    act(std::string_view id, SessionAction action, bool allowInteractive, const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Calls.push_back(std::string(action == SessionAction::Lock ? "lock " : "signout ") + std::string(id) +
                          (allowInteractive ? " interactive" : ""));
        if (id == "5" && !allowInteractive)
        {
            Platform::ServiceActionResult refused = Platform::ServiceActionResult::failed("Requires administrator");
            refused.needsAuthorization = true;
            return refused;
        }
        return Platform::ServiceActionResult::succeeded();
    }
    [[nodiscard]] std::vector<std::string> calls()
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Calls;
    }
    std::string m_Failure;
    std::atomic<int> m_Reads{0};

  private:
    std::vector<UserSession> m_Sessions;
    bool m_Has = true;
    std::mutex m_Mutex;
    std::vector<std::string> m_Calls;
};

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
    UsersPanel panel([fake] { return fake; }, [](Core::Event&) {});
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

TEST_F(UsersPanelRenderTest, UnsupportedFailedAndEmptyReadsSaySo)
{
    auto unsupported = std::make_shared<FakeSessions>(std::vector<UserSession>{}, false);
    UsersPanel none([unsupported] { return unsupported; }, [](Core::Event&) {});
    none.onAttach();
    show(none, "Users");
    EXPECT_TRUE(frame(none).contains("Users aren't available"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(unsupported->m_Reads.load(), 0);
    none.onDetach();

    auto failing = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    failing->m_Failure = "systemd-logind isn't running";
    UsersPanel failed([failing] { return failing; }, [](Core::Event&) {});
    failed.onAttach();
    show(failed, "Users");
    const std::string text = frameUntil(failed, "couldn't be read");
    EXPECT_TRUE(text.contains("systemd-logind isn't running")) << text;
    Core::RefreshRateChangedEvent faster(250);
    failed.onEvent(faster);
    failed.onDetach();

    auto empty = std::make_shared<FakeSessions>(std::vector<UserSession>{});
    UsersPanel nobody([empty] { return empty; }, [](Core::Event&) {});
    nobody.onAttach();
    show(nobody, "Users");
    EXPECT_TRUE(frameUntil(nobody, "No one is signed in").contains("No one is signed in"));
    show(nobody, "Processes");
    show(nobody, "Users"); // shown again: the running sampler reads at once
    nobody.onDetach();
}

} // namespace
} // namespace App
