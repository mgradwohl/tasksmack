/// @file test_Users.cpp
/// @brief The Users tab (#3076): a row per signed-in user with their sessions and their processes' totals,
/// the session text, the model's order, Lock and Sign out (the confirm, the worker, Authenticate and retry),
/// and the panel, sampled only once shown, its rows and its session menu.

#include "App/Panels/SessionActionControl.h"
#include "App/Panels/UsersPanel.h"
#include "App/Panels/UsersView.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/UsersModel.h"
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
#include <cstddef>
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

// #3076: Windows' sessions say where they are (the console, a Remote Desktop and its client), the WTS states
// it adds read as words, and the tooltip names the domain, the sign-in and the session.
TEST(UserRowsTest, WindowsSessionsSayWhereTheyAreAndWhatTheyAreDoing)
{
    UserSession console = session("1", "alice", 0);
    console.type = "console";
    EXPECT_EQ(sessionPlace(console), "Console");
    UserSession rdp = session("3", "bob", 0);
    rdp.seat = "RDP-Tcp#0";
    rdp.type = "rdp";
    rdp.remote = true;
    rdp.remoteHost = "LAPTOP";
    EXPECT_EQ(sessionPlace(rdp), "RDP-Tcp#0 \xC2\xB7 Remote Desktop \xC2\xB7 from LAPTOP");
    UserSession away = session("4", "carol", 0, "disconnected");
    EXPECT_EQ(sessionPlace(away), "Session 4") << "a disconnected session has no client to say";

    EXPECT_EQ(sessionStatus(away, 0), "Disconnected");
    EXPECT_EQ(sessionStatus(session("5", "dave", 0, "connecting"), 0), "Connecting");

    EXPECT_EQ(sessionTooltip(session("2", "alice", 1000)), "alice\nSession 2") << "Linux: no domain, no sign-in time";
    UserSession signedIn = session("1", "alice", 0);
    signedIn.domain = "PC";
    signedIn.logonUsec = 1'700'000'000'000'000ULL;
    const std::string tooltip = sessionTooltip(signedIn);
    EXPECT_TRUE(tooltip.starts_with("PC\\alice\nSigned in ")) << tooltip;
    EXPECT_TRUE(tooltip.ends_with("\nSession 1")) << tooltip;

    EXPECT_FALSE(SessionMenu{}.any());
    EXPECT_TRUE((SessionMenu{.lock = false, .disconnect = true, .signOut = false}.any()));
}

// #3076: two accounts with one short name (PC\\alice and CORP\\alice) are two rows, each labelled with its
// domain. A process that names only the short name can't be told apart, so neither row gets it; a name only
// one row has keeps its processes.
TEST(UserRowsTest, AccountsOfTwoDomainsAreTwoRows)
{
    UserSession local = session("1", "alice", 0);
    local.domain = "PC";
    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    UserSession bob = session("3", "bob", 0);
    bob.domain = "PC";
    UserSession again = session("4", "bob", 0);
    again.domain = "PC";
    const std::vector<Domain::ProcessSnapshot> processes{process(10, "alice", 5.0, 100), process(11, "bob", 1.0, 50)};
    const std::vector<UserRow> rows = buildUserRows({local, corp, bob, again}, processes);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].label, "bob") << "a short name only one row has";
    EXPECT_EQ(rows[0].key, "PC\\bob");
    EXPECT_EQ(rows[0].sessions.size(), 2U) << "both of PC\\bob's sessions";
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{1}));
    EXPECT_EQ(rows[1].label, "CORP\\alice");
    EXPECT_EQ(rows[2].label, "PC\\alice");
    EXPECT_TRUE(rows[1].processes.empty()) << "an alice process could be either alice's";
    EXPECT_TRUE(rows[2].processes.empty());
    EXPECT_EQ(rows[1].user, "alice");
}

/// A Windows process, which names its owner's domain too (#3310).
[[nodiscard]] Domain::ProcessSnapshot processOf(std::int32_t pid, std::string domain, std::string user, double cpu)
{
    Domain::ProcessSnapshot p = process(pid, std::move(user), cpu, 100);
    p.userDomain = std::move(domain);
    return p;
}

// #3310: a Windows process names its owner's domain, so PC\\alice's and CORP\\alice's processes each go to
// their own row, whatever the case of the names. One with no domain still goes only to a name one row has.
TEST(UserRowsTest, AProcessWithItsDomainGoesToThatAccountsRow)
{
    UserSession local = session("1", "alice", 0);
    local.domain = "PC";
    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    UserSession bob = session("3", "bob", 0);
    bob.domain = "PC";
    const std::vector<Domain::ProcessSnapshot> processes{
        processOf(10, "PC", "alice", 5.0),
        processOf(11, "CORP", "alice", 7.0),
        processOf(12, "corp", "ALICE", 1.0), // Windows compares account names without case
        processOf(13, "NT AUTHORITY", "SYSTEM", 50.0),
        processOf(14, "OTHER", "alice", 9.0), // A third alice, signed in nowhere
        process(15, "alice", 3.0, 100),       // No domain: either alice's
        process(16, "bob", 2.0, 100),         // No domain, a name one row has
        processOf(17, "CORP", "bob", 4.0),    // CORP\\bob isn't PC\\bob
    };
    const std::vector<UserRow> rows = buildUserRows({local, corp, bob}, processes);
    ASSERT_EQ(rows.size(), 3U);
    EXPECT_EQ(rows[0].key, "PC\\bob");
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{6}));
    EXPECT_EQ(rows[1].key, "CORP\\alice");
    EXPECT_EQ(rows[1].processes, (std::vector<std::size_t>{1, 2})) << "busiest first";
    EXPECT_EQ(rows[1].totals.processes, 2U);
    EXPECT_EQ(rows[2].key, "PC\\alice");
    EXPECT_EQ(rows[2].processes, (std::vector<std::size_t>{0}));
}

// #3310: a row whose sessions gave no domain takes a process with one when only that row has its name (a
// session WTS named no domain for can't be told from the process's account). Linux's rows and processes,
// with no domain on either side, match by name as they did.
TEST(UserRowsTest, ARowWithNoDomainTakesItsNamesProcesses)
{
    const std::vector<Domain::ProcessSnapshot> processes{processOf(10, "PC", "alice", 5.0), process(11, "alice", 1.0, 100)};
    const std::vector<UserRow> rows = buildUserRows({session("1", "alice", 1000)}, processes);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows[0].processes, (std::vector<std::size_t>{0, 1}));

    UserSession corp = session("2", "alice", 0);
    corp.domain = "CORP";
    const std::vector<UserRow> two = buildUserRows({session("1", "alice", 1000), corp}, processes);
    ASSERT_EQ(two.size(), 2U);
    EXPECT_TRUE(two[0].processes.empty()) << "alice has two rows: PC\\alice's process could be the bare row's";
    EXPECT_TRUE(two[1].processes.empty());
}

// #3076: a Windows session whose user couldn't be read is a row of its own, said to be not readable, with no
// processes and its reason in the tooltip.
TEST(UserRowsTest, ASessionNotReadableIsItsOwnRow)
{
    UserSession hidden = session("5", "", 0, "disconnected");
    hidden.unreadable = "Access is denied";
    const std::vector<UserRow> rows = buildUserRows({hidden, session("1", "alice", 1000)}, {process(10, "", 1.0, 1)});
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows[1].label, "Not readable");
    EXPECT_TRUE(rows[1].user.empty());
    EXPECT_TRUE(rows[1].processes.empty());
    EXPECT_EQ(sessionTooltip(hidden), "Its user couldn't be read: Access is denied\nSession 5");
}

/// Sessions a test controls: what read() lists and what act() answers, with each call recorded. Linux's
/// actions (Lock and Sign out), or with @p windows Windows' (Disconnect and Sign out).
class FakeSessions final : public Platform::IUserSessions
{
  public:
    explicit FakeSessions(std::vector<UserSession> sessions, bool has = true, bool windows = false)
        : m_Sessions(std::move(sessions)), m_Has(has), m_Windows(windows)
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
    [[nodiscard]] bool canAct(SessionAction action) const override
    {
        return m_Has && (action == SessionAction::SignOut || (action == SessionAction::Disconnect) == m_Windows);
    }
    [[nodiscard]] Platform::ServiceActionResult
    act(std::string_view id, SessionAction action, bool allowInteractive, const std::stop_token& /*stopToken*/) override
    {
        const std::scoped_lock lock(m_Mutex);
        m_Calls.push_back(std::string(actionName(action)) + " " + std::string(id) + (allowInteractive ? " interactive" : ""));
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
    [[nodiscard]] static const char* actionName(SessionAction action)
    {
        switch (action)
        {
        case SessionAction::Lock:
            return "lock";
        case SessionAction::Disconnect:
            return "disconnect";
        case SessionAction::SignOut:
            break;
        }
        return "signout";
    }

    std::vector<UserSession> m_Sessions;
    bool m_Has = true;
    bool m_Windows = false;
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
    const std::string question = disconnectQuestion(remote);
    EXPECT_TRUE(question.contains("the Remote Desktop connection closes")) << question;
    EXPECT_FALSE(question.contains("this screen locks")) << question;

    EXPECT_STREQ(sessionActionWords(SessionAction::Lock).done, "Locked");
    EXPECT_STREQ(sessionActionWords(SessionAction::Disconnect).title, "Disconnect");
    EXPECT_STREQ(sessionActionWords(SessionAction::SignOut).title, "Sign out");
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

// #3076 (Windows): the session menu has Disconnect... and Sign out..., never Lock. Disconnect... opens its own
// confirm, and is greyed for a session that is already disconnected.
TEST_F(UsersPanelRenderTest, WindowsSessionMenusOfferDisconnectInLocksPlace)
{
    UserSession console = session("1", "alice", 0);
    console.type = "console";
    console.current = true;
    UserSession away = session("4", "bob", 0, "disconnected");
    auto fake = std::make_shared<FakeSessions>(std::vector<UserSession>{console, away}, true, true);
    UsersPanel panel([fake] { return fake; }, [](Core::Event&) {});
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
    UsersPanel panel([fake] { return fake; },
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
