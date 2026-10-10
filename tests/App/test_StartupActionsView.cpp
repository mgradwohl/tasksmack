/// @file test_StartupActionsView.cpp
/// @brief The Startup tab's actions (#801, phase 2): which rows can be enabled or disabled (RunOnce
/// never, all-users entries only elevated), the texts, and, headless, the row menu enabling its items
/// per row, Disable confirming first, Enable running straight away, and the result line. The actions
/// are a fake: no real startup entry is touched.

#include "App/Panels/StartupActionsView.h"
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
    e.scope =
        (location == StartupLocation::RunUser || location == StartupLocation::RunOnceUser || location == StartupLocation::StartupFolderUser)
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
    EXPECT_FALSE(AutoDetail::isApplicable(false, user, Platform::StartupActionCapabilities{}));
}

TEST(StartupActionsDetailTest, TextsNameTheEntryAndTheOutcome)
{
    const StartupActionRequest disable{.enable = false, .entry = entry("OneDrive", StartupLocation::RunUser)};
    EXPECT_TRUE(AutoDetail::needsConfirm(disable));
    EXPECT_FALSE(AutoDetail::needsConfirm({.enable = true, .entry = disable.entry}));
    EXPECT_EQ(AutoDetail::confirmTitle(disable), "Disable OneDrive?");
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

} // namespace
} // namespace App
