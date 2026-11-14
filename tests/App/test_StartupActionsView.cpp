/// @file test_StartupActionsView.cpp
/// @brief The Startup tab's actions (#801, phase 2): which rows can be enabled or disabled (RunOnce
/// never, all-users entries only elevated), the texts, and, headless, the row menu enabling its items
/// per row, Disable confirming first, Enable running straight away, and the result line. The actions
/// are a fake: no real startup entry is touched.

#include "App/Panels/StartupActionsView.h"
#include "App/SessionAccountCheck.h"
#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // OpenPopupStack, ImHashStr(), ActivateItemByID()

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace App
{
namespace
{

using Platform::StartupLocation;
namespace AutoDetail = StartupActionsDetail;

constexpr Platform::StartupActionCapabilities USER{.canSetEnabled = true, .elevated = false};
constexpr Platform::StartupActionCapabilities ADMIN{.canSetEnabled = true, .elevated = true};

/// Answers every action with `result`, counting the calls (made on the action worker).
class FakeStartupActions final : public Platform::IStartupActions
{
  public:
    explicit FakeStartupActions(Platform::StartupActionCapabilities caps = USER) : m_Caps(caps)
    {}

    Platform::StartupActionResult result = Platform::StartupActionResult::succeeded();
    std::atomic<int> enables{0};
    std::atomic<int> disables{0};

    [[nodiscard]] Platform::StartupActionCapabilities capabilities() const override
    {
        return m_Caps;
    }
    [[nodiscard]] Platform::StartupActionResult setEnabled(const Platform::StartupEntry& /*entry*/, bool enabled) override
    {
        ++(enabled ? enables : disables);
        return result;
    }

  private:
    Platform::StartupActionCapabilities m_Caps;
};

/// Throws from every action: a std::runtime_error("boom"), or with `nonStandard` an int.
class ThrowingStartupActions final : public Platform::IStartupActions
{
  public:
    explicit ThrowingStartupActions(bool nonStandard = false) : m_NonStandard(nonStandard)
    {}

    [[nodiscard]] Platform::StartupActionCapabilities capabilities() const override
    {
        return USER;
    }
    [[nodiscard]] Platform::StartupActionResult setEnabled(const Platform::StartupEntry& /*entry*/, bool /*enabled*/) override
    {
        if (m_NonStandard)
        {
            // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally not a std::exception
            throw 42;
        }
        throw std::runtime_error("boom");
    }

  private:
    bool m_NonStandard;
};

[[nodiscard]] Platform::StartupEntry entry(const char* name, StartupLocation location, bool enabled = true)
{
    Platform::StartupEntry e;
    e.name = name;
    e.location = location;
    e.scope = (location == StartupLocation::RunUser || location == StartupLocation::RunOnceUser ||
               location == StartupLocation::StartupFolderUser || location == StartupLocation::PackagedApp)
                ? Platform::StartupScope::User
                : Platform::StartupScope::Machine;
    e.enabled = enabled;
    return e;
}

/// Waits (briefly: the fake answers at once) for the worker, as the panel's per-frame poll does.
[[nodiscard]] bool waitFinished(StartupActionsView& view)
{
    for (int i = 0; i < 300; ++i)
    {
        if (view.takeFinished())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

TEST(StartupActionsDetailTest, WhichRowsCanChange)
{
    const auto user = entry("OneDrive", StartupLocation::RunUser);
    EXPECT_TRUE(AutoDetail::isApplicable(false, user, USER));
    EXPECT_FALSE(AutoDetail::isApplicable(true, user, USER)); // already enabled
    EXPECT_TRUE(AutoDetail::isApplicable(true, entry("OneDrive", StartupLocation::RunUser, false), USER));

    const auto machine = entry("Helper", StartupLocation::RunMachine);
    EXPECT_EQ(AutoDetail::blockedReason(machine, USER), "Requires administrator");
    EXPECT_FALSE(AutoDetail::isApplicable(false, machine, USER));
    EXPECT_TRUE(AutoDetail::isApplicable(false, machine, ADMIN));

    const auto once = entry("Setup", StartupLocation::RunOnceUser);
    EXPECT_TRUE(AutoDetail::blockedReason(once, ADMIN).contains("Run-once"));
    EXPECT_FALSE(AutoDetail::isApplicable(false, once, ADMIN));

    // A packaged app's startup task can be changed for the current user (#1873), unless a policy sets it.
    const auto packaged = entry("Microsoft Teams", StartupLocation::PackagedApp);
    EXPECT_TRUE(AutoDetail::blockedReason(packaged, USER).empty());
    EXPECT_TRUE(AutoDetail::isApplicable(false, packaged, USER));
    EXPECT_TRUE(AutoDetail::isApplicable(true, entry("Spotify", StartupLocation::PackagedApp, false), USER));
    auto managed = entry("Company Portal", StartupLocation::PackagedApp);
    managed.setByPolicy = true;
    EXPECT_TRUE(AutoDetail::blockedReason(managed, ADMIN).contains("policy"));
    EXPECT_FALSE(AutoDetail::isApplicable(false, managed, ADMIN));
    EXPECT_FALSE(Platform::hasSwitchableState(managed));
    EXPECT_FALSE(AutoDetail::isApplicable(false, user, Platform::StartupActionCapabilities{}));
}

// #2541: Linux's XDG autostart. A system entry is switched by the user's own copy, so it needs no root;
// one for another desktop says why it can't be switched.
TEST(StartupActionsDetailTest, XdgAutostartEntries)
{
    const Platform::StartupActionCapabilities linux{.canSetEnabled = true, .elevated = false, .machineEntriesNeedElevation = false};
    auto system = entry("Updater", StartupLocation::XdgAutostartSystem);
    system.scope = Platform::StartupScope::Machine;
    EXPECT_TRUE(AutoDetail::blockedReason(system, linux).empty());
    EXPECT_TRUE(AutoDetail::isApplicable(false, system, linux));
    auto other = entry("KDE Wallet", StartupLocation::XdgAutostartUser, false);
    other.notSwitchableReason = "Starts only in KDE";
    EXPECT_EQ(AutoDetail::blockedReason(other, linux), "Starts only in KDE");
    EXPECT_FALSE(AutoDetail::isApplicable(true, other, linux));
    EXPECT_FALSE(Platform::hasSwitchableState(other));
    EXPECT_TRUE(AutoDetail::confirmQuestion({.enable = false, .entry = system}).contains("autostart file is kept"));
}

// #3424: Windows' scheduled tasks. The user's own switch without elevation; another account's needs it; Windows'
// own are read-only, whoever asks; disabling one stops it on every trigger, which the confirm says.
TEST(StartupActionsDetailTest, ScheduledTasks)
{
    auto mine = entry("Updater", StartupLocation::ScheduledTaskUser);
    mine.scope = Platform::StartupScope::User; // a task that runs as the current user
    EXPECT_TRUE(AutoDetail::blockedReason(mine, USER).empty());
    EXPECT_TRUE(AutoDetail::isApplicable(false, mine, USER));
    const auto system = entry("Agent", StartupLocation::ScheduledTaskMachine);
    EXPECT_EQ(AutoDetail::blockedReason(system, USER), "Requires administrator");
    EXPECT_TRUE(AutoDetail::isApplicable(false, system, ADMIN));
    auto windows = entry("Cleanup", StartupLocation::ScheduledTaskMachine);
    windows.osComponent = true;
    windows.notSwitchableReason = "One of Windows' own scheduled tasks";
    EXPECT_EQ(AutoDetail::blockedReason(windows, ADMIN), "One of Windows' own scheduled tasks");
    EXPECT_FALSE(AutoDetail::isApplicable(false, windows, ADMIN));
    const std::string question = AutoDetail::confirmQuestion({.enable = false, .entry = mine});
    EXPECT_TRUE(question.contains("on any other trigger")) << question;
    EXPECT_TRUE(question.contains("kept in Task Scheduler")) << question;
}

TEST(StartupActionsDetailTest, TextsNameTheEntryAndTheOutcome)
{
    const StartupActionRequest disable{.enable = false, .entry = entry("OneDrive", StartupLocation::RunUser)};
    EXPECT_TRUE(AutoDetail::needsConfirm(disable));
    EXPECT_FALSE(AutoDetail::needsConfirm({.enable = true, .entry = disable.entry}));
    EXPECT_EQ(AutoDetail::confirmTitle(disable), "Disable OneDrive?");
    EXPECT_TRUE(AutoDetail::confirmQuestion(disable).contains("registry entry or shortcut is kept"));
    const StartupActionRequest packaged{.enable = false, .entry = entry("Spotify", StartupLocation::PackagedApp)};
    EXPECT_TRUE(AutoDetail::confirmQuestion(packaged).contains("startup task are kept")) << "#1873";
    EXPECT_EQ(AutoDetail::progressText(disable), "Disabling OneDrive...");
    EXPECT_EQ(AutoDetail::resultMessage(disable, Platform::StartupActionResult::succeeded()).text, "Disabled OneDrive");
    const auto denied = AutoDetail::resultMessage(disable, Platform::StartupActionResult::failed("Requires administrator"));
    EXPECT_FALSE(denied.ok);
    EXPECT_EQ(denied.text, "Could not disable OneDrive: Requires administrator");
}

TEST(StartupActionsViewTest, ActionThatThrowsBecomesAPlainFailedResult)
{
    StartupActionsView view(std::make_shared<ThrowingStartupActions>());
    view.request({.enable = true, .entry = entry("OneDrive", StartupLocation::RunUser, false)});
    ASSERT_TRUE(waitFinished(view));
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not enable OneDrive: boom");
    EXPECT_FALSE(view.busy());
}

TEST(StartupActionsViewTest, ActionThatThrowsANonStandardExceptionBecomesAGenericFailedResult)
{
    StartupActionsView view(std::make_shared<ThrowingStartupActions>(true));
    view.request({.enable = true, .entry = entry("OneDrive", StartupLocation::RunUser, false)});
    ASSERT_TRUE(waitFinished(view));
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not enable OneDrive: Unknown error");
}

// #1891: a request while an action is still running (its result not yet taken) is ignored rather than
// queued, and the result line goes once its RESULT_SECONDS are up.
TEST(StartupActionsViewTest, ARequestWhileBusyIsIgnoredAndTheResultExpires)
{
    const auto fake = std::make_shared<FakeStartupActions>();
    StartupActionsView view(fake);
    view.request({.enable = true, .entry = entry("OneDrive", StartupLocation::RunUser, false)});
    EXPECT_TRUE(view.busy());
    view.request({.enable = true, .entry = entry("Teams", StartupLocation::RunUser, false)}); // Ignored: busy
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 1);
    EXPECT_EQ(view.lastResult().text, "Enabled OneDrive");

    view.tick(AutoDetail::RESULT_SECONDS - 1.0F);
    EXPECT_EQ(view.lastResult().text, "Enabled OneDrive"); // Still showing
    view.tick(2.0F);
    EXPECT_TRUE(view.lastResult().text.empty());
}

// #3671: in a TaskSmack elevated as another account, the per-user entries are that account's and read-only; the
// all-users ones stay as they are (an elevated copy changes them).
TEST(StartupActionsDetailTest, AnotherAccountsPerUserEntriesAreReadOnly)
{
    Platform::StartupActionCapabilities other = ADMIN;
    other.perUserEntriesOtherAccount = true;
    for (const StartupLocation location :
         {StartupLocation::RunUser, StartupLocation::StartupFolderUser, StartupLocation::PackagedApp, StartupLocation::ScheduledTaskUser})
    {
        auto perUser = entry("OneDrive", location);
        perUser.scope = Platform::StartupScope::User;
        EXPECT_EQ(AutoDetail::blockedReason(perUser, other), AutoDetail::OTHER_ACCOUNT_REASON);
        EXPECT_FALSE(AutoDetail::isApplicable(false, perUser, other));
        EXPECT_TRUE(AutoDetail::isApplicable(false, perUser, ADMIN)) << "the signed-in user's own elevated copy changes it";
    }
    EXPECT_TRUE(AutoDetail::blockedReason(entry("Helper", StartupLocation::RunMachine), other).empty());
    EXPECT_TRUE(AutoDetail::isApplicable(false, entry("Helper", StartupLocation::RunMachine), other));
    EXPECT_EQ(AutoDetail::otherAccountScopeLabel("admin"), "admin (administrator)");
    const std::string note = AutoDetail::otherAccountNote("admin");
    EXPECT_TRUE(note.contains("runs as admin, not as you")) << note;
    EXPECT_TRUE(note.contains("normal TaskSmack")) << note;
}

// #3682 (Linux): where an all-users entry's switch is the running account's own file (machineEntriesNeedElevation
// false: /etc/xdg/autostart switched by a copy in ~/.config/autostart), a TaskSmack running as another account leaves
// those alone too, saying why. Windows' all-users entries (the machine's own setting) are unchanged above.
TEST(StartupActionsDetailTest, AnotherAccountsSwitchOfAnAllUsersEntryIsReadOnlyWhereItIsAPerUserFile)
{
    const Platform::StartupActionCapabilities linux{.canSetEnabled = true, .elevated = false, .machineEntriesNeedElevation = false};
    Platform::StartupActionCapabilities other = linux;
    other.perUserEntriesOtherAccount = true;

    Platform::StartupEntry system = entry("Tracker", StartupLocation::XdgAutostartSystem);
    system.scope = Platform::StartupScope::Machine;
    EXPECT_TRUE(AutoDetail::switchIsAccounts(system, linux));
    EXPECT_TRUE(AutoDetail::isApplicable(false, system, linux)) << "the user's own TaskSmack switches it";
    EXPECT_EQ(AutoDetail::blockedReason(system, other), AutoDetail::OTHER_ACCOUNT_SWITCH_REASON);
    EXPECT_FALSE(AutoDetail::isApplicable(false, system, other));

    Platform::StartupEntry user = entry("Syncthing", StartupLocation::XdgAutostartUser);
    user.scope = Platform::StartupScope::User;
    EXPECT_EQ(AutoDetail::blockedReason(user, other), AutoDetail::OTHER_ACCOUNT_REASON);

    // An entry that can't be switched anyway keeps its own reason.
    Platform::StartupEntry unit = entry("backup.timer", StartupLocation::SystemdSystemUnit);
    unit.scope = Platform::StartupScope::Machine;
    unit.notSwitchableReason = "Enabled for every user";
    EXPECT_EQ(AutoDetail::blockedReason(unit, other), "Enabled for every user");

    EXPECT_FALSE(AutoDetail::switchIsAccounts(entry("Helper", StartupLocation::RunMachine), ADMIN)) << "Windows: the machine's";
}

// #3682: the worker refuses such an entry too, even when asked for before the account check answered.
TEST(StartupActionsViewTest, AnotherAccountsSwitchOfAnAllUsersEntryIsNeverWrittenWhereItIsAPerUserFile)
{
    const auto fake = std::make_shared<FakeStartupActions>(
        Platform::StartupActionCapabilities{.canSetEnabled = true, .elevated = false, .machineEntriesNeedElevation = false});
    StartupActionsView view(fake, SessionAccountCheck::known({.otherAccount = true, .account = "root"}));
    Platform::StartupEntry system = entry("Tracker", StartupLocation::XdgAutostartSystem, false);
    system.scope = Platform::StartupScope::Machine;
    view.request({.enable = true, .entry = system}); // before the answer is taken in
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 0);
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_TRUE(view.lastResult().text.contains(AutoDetail::OTHER_ACCOUNT_SWITCH_REASON)) << view.lastResult().text;
}

// The view takes the check's answer once it has one: the Scope label and the read-only per-user entries. An action
// asked for before then still waits for the answer on the worker, and another account's entry is never changed.
TEST(StartupActionsViewTest, AnotherAccountsPerUserEntriesAreNeverChanged)
{
    const auto fake = std::make_shared<FakeStartupActions>(ADMIN);
    StartupActionsView view(fake, SessionAccountCheck::known({.otherAccount = true, .account = "admin"}));
    EXPECT_TRUE(view.perUserScopeLabel().empty()) << "not taken in yet";
    view.request({.enable = true, .entry = entry("OneDrive", StartupLocation::RunUser, false)}); // before the answer
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 0);
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_TRUE(view.lastResult().text.contains(AutoDetail::OTHER_ACCOUNT_REASON)) << view.lastResult().text;

    view.pollAccount();
    EXPECT_EQ(view.perUserScopeLabel(), "admin (administrator)");
    EXPECT_TRUE(view.capabilities().perUserEntriesOtherAccount);

    view.request({.enable = true, .entry = entry("Helper", StartupLocation::RunMachine, false)}); // all users: as before
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 1);
    EXPECT_TRUE(view.lastResult().ok);
}

TEST(StartupActionsViewTest, TheSignedInUsersTaskSmackChangesItsOwnEntries)
{
    const auto fake = std::make_shared<FakeStartupActions>(ADMIN);
    StartupActionsView view(fake, SessionAccountCheck::known({}));
    view.pollAccount();
    EXPECT_TRUE(view.perUserScopeLabel().empty());
    EXPECT_FALSE(view.capabilities().perUserEntriesOtherAccount);
    view.request({.enable = true, .entry = entry("OneDrive", StartupLocation::RunUser, false)});
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 1);
}

TEST(StartupActionsViewTest, UnsupportedPlatformHasNoActions)
{
    const StartupActionsView view(std::make_shared<Platform::UnsupportedStartupActions>());
    EXPECT_FALSE(view.supported());
}

class StartupActionsViewRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1200.0F, 800.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of the Startup window: a row for @p row with its menu (opened when @p openMenu), the
    /// action bar, the result line and the confirm, as StartupView and StartupPanel draw them.
    static void frame(StartupActionsView& view, const Platform::StartupEntry& row, bool openMenu = false)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
        ImGui::Begin("Startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        view.renderActionBar(&row);
        view.renderResultLine();
        ImGui::PushID(row.name.c_str());
        ImGui::Selectable(row.name.c_str());
        if (openMenu)
        {
            ImGui::OpenPopup("##StartupRowMenu");
        }
        view.renderContextMenu(row);
        ImGui::PopID();
        view.renderConfirmation();
        ImGui::End();
        ImGui::Render();
    }

    /// Opens @p row's menu and picks its item labelled @p label: a disabled item does nothing.
    static void pickMenuItem(StartupActionsView& view, const Platform::StartupEntry& row, const char* label)
    {
        frame(view, row, true);
        frame(view, row);
        const ImGuiContext& g = *ImGui::GetCurrentContext();
        ASSERT_FALSE(g.OpenPopupStack.empty());
        ImGui::ActivateItemByID(ImHashStr(label, 0, g.OpenPopupStack.back().Window->ID));
        frame(view, row);
        frame(view, row);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(StartupActionsViewRenderTest, DisableFromTheMenuConfirmsFirst)
{
    const auto fake = std::make_shared<FakeStartupActions>();
    StartupActionsView view(fake);
    const auto enabled = entry("OneDrive", StartupLocation::RunUser);

    pickMenuItem(view, enabled, ICON_FA_CIRCLE_CHECK "  Enable"); // disabled: it is enabled
    EXPECT_FALSE(view.busy());
    EXPECT_FALSE(view.confirmRequested());

    pickMenuItem(view, enabled, ICON_FA_POWER_OFF "  Disable");
    EXPECT_TRUE(view.confirmRequested());
    EXPECT_FALSE(view.pendingConfirm().value_or(StartupActionRequest{.enable = true, .entry = {}}).enable);
    EXPECT_EQ(fake->disables.load(), 0);

    // A danger-filled button: its ID is "##filled" under the label (UI::Widgets::filledButton()).
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string(modal->Name).starts_with("Disable OneDrive?"));
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Disable", 0, modal->ID)));
    frame(view, enabled);
    frame(view, enabled);
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->disables.load(), 1);
    EXPECT_EQ(view.lastResult().text, "Disabled OneDrive");
}

TEST_F(StartupActionsViewRenderTest, EnableRunsStraightAwayAndShowsTheResult)
{
    const auto fake = std::make_shared<FakeStartupActions>();
    fake->result = Platform::StartupActionResult::failed("Windows error 87");
    StartupActionsView view(fake);
    const auto disabled = entry("OneDrive", StartupLocation::RunUser, false);

    pickMenuItem(view, disabled, ICON_FA_CIRCLE_CHECK "  Enable");
    EXPECT_FALSE(view.confirmRequested());
    ASSERT_TRUE(waitFinished(view));
    EXPECT_EQ(fake->enables.load(), 1);
    EXPECT_FALSE(view.lastResult().ok);
    EXPECT_EQ(view.lastResult().text, "Could not enable OneDrive: Windows error 87");
    frame(view, disabled); // the result line draws
    EXPECT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);

    view.tick(AutoDetail::RESULT_SECONDS + 1.0F);
    EXPECT_TRUE(view.lastResult().text.empty());
}

TEST_F(StartupActionsViewRenderTest, BlockedRowsOfferNoAction)
{
    const auto fake = std::make_shared<FakeStartupActions>(); // not elevated
    StartupActionsView view(fake);
    for (const auto& row : {entry("Helper", StartupLocation::RunMachine), entry("Setup", StartupLocation::RunOnceUser)})
    {
        pickMenuItem(view, row, ICON_FA_POWER_OFF "  Disable");
        EXPECT_FALSE(view.confirmRequested());
        EXPECT_FALSE(view.busy());
    }
    EXPECT_EQ(fake->disables.load(), 0);
}

// #3671: another account's per-user entries offer no action, in the menu or the bar, and the note says whose
// they are; an all-users entry is offered as before.
TEST_F(StartupActionsViewRenderTest, AnotherAccountsPerUserRowsOfferNoAction)
{
    const auto fake = std::make_shared<FakeStartupActions>(ADMIN);
    StartupActionsView view(fake, SessionAccountCheck::known({.otherAccount = true, .account = "admin"}));
    view.pollAccount();
    const auto perUser = entry("OneDrive", StartupLocation::RunUser);
    pickMenuItem(view, perUser, ICON_FA_POWER_OFF "  Disable");
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_FALSE(view.busy());
    EXPECT_EQ(fake->disables.load(), 0);

    pickMenuItem(view, entry("Helper", StartupLocation::RunMachine), ICON_FA_POWER_OFF "  Disable");
    EXPECT_TRUE(view.confirmRequested()) << "an all-users entry is the elevated copy's to change";

    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
    ImGui::Begin("Startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const float before = ImGui::GetCursorPosY();
    view.renderAccountNote();
    const float after = ImGui::GetCursorPosY();
    ImGui::End();
    ImGui::Render();
    EXPECT_GT(after, before) << "the note draws a line";

    StartupActionsView own(fake, SessionAccountCheck::known({}));
    own.pollAccount();
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(1200.0F, 800.0F));
    ImGui::Begin("Startup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const float ownBefore = ImGui::GetCursorPosY();
    own.renderAccountNote();
    EXPECT_FLOAT_EQ(ImGui::GetCursorPosY(), ownBefore) << "no note for the signed-in user's own TaskSmack";
    ImGui::End();
    ImGui::Render();
}

} // namespace
} // namespace App
