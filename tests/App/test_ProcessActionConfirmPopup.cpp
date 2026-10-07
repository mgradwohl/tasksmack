/// @file test_ProcessActionConfirmPopup.cpp
/// @brief The confirm modal's real ImGui lifecycle, headless (Copilot review on #1447): clearing the
/// request flag alone leaves an open modal open, `dismiss` closes it, and ProcessActionsView closes
/// it unconfirmed after a selection change, or when the live target moved, so a confirm can never act
/// on a process other than the one it was requested for.

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <functional>

namespace App
{
namespace
{

using Detail::ProcessAction;

// The popup's ID: the "###" part of its title, as ProcessActionConfirm.cpp names it.
constexpr const char* CONFIRM_POPUP_ID = "###ConfirmAction";

constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};

constexpr Platform::ProcessActionCapabilities ALL_ACTIONS{
    .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true};

class ProcessActionConfirmPopupTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1600.0F, 1000.0F);
        io.DeltaTime = 1.0F / 60.0F;
        // No renderer: let ImGui build and own the font atlas itself.
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a window like the Process Details pane, running @p body inside it; returns whether
    /// the confirm modal is open at the end of the frame, asked from that window as the code under
    /// test asks it.
    static bool runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        const bool open = ImGui::IsPopupOpen(CONFIRM_POPUP_ID);
        ImGui::End();
        ImGui::Render();
        return open;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessActionConfirmPopupTest, ClearingTheFlagAloneLeavesAnOpenModalOpen)
{
    // The behaviour the review found: the flag only opens the modal, it does not close it.
    bool show = true;
    EXPECT_TRUE(runFrame([&] { (void) ProcessActionConfirm::render(show, ProcessAction::Kill, "a", TARGET_A.pid); }));
    show = false;
    EXPECT_TRUE(runFrame([&] { (void) ProcessActionConfirm::render(show, ProcessAction::Kill, "b", TARGET_B.pid); }));
}

TEST_F(ProcessActionConfirmPopupTest, DismissClosesAnOpenModalUnconfirmed)
{
    bool show = true;
    ASSERT_TRUE(runFrame([&] { (void) ProcessActionConfirm::render(show, ProcessAction::Kill, "a", TARGET_A.pid); }));

    auto outcome = ProcessActionConfirm::Outcome::None;
    EXPECT_FALSE(runFrame([&] { outcome = ProcessActionConfirm::render(show, ProcessAction::Kill, "a", TARGET_A.pid, true); }));
    EXPECT_EQ(outcome, ProcessActionConfirm::Outcome::Cancelled);
    EXPECT_FALSE(show);
    EXPECT_FALSE(runFrame([&] { (void) ProcessActionConfirm::render(show, ProcessAction::Kill, "a", TARGET_A.pid); }));
}

TEST_F(ProcessActionConfirmPopupTest, DismissWithNothingOpenDoesNothing)
{
    bool show = false;
    auto outcome = ProcessActionConfirm::Outcome::Cancelled;
    EXPECT_FALSE(runFrame([&] { outcome = ProcessActionConfirm::render(show, ProcessAction::Kill, "a", TARGET_A.pid, true); }));
    EXPECT_EQ(outcome, ProcessActionConfirm::Outcome::None);
}

TEST_F(ProcessActionConfirmPopupTest, ViewClosesTheModalWhenTheSelectionChanges)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    ASSERT_TRUE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "a", TARGET_A); }));

    view.onSelectionChanged(); // B is selected
    EXPECT_FALSE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "b", TARGET_B); }));
    EXPECT_FALSE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "b", TARGET_B); }));
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_EQ(mock.killCount(), 0);
}

TEST_F(ProcessActionConfirmPopupTest, AModalLeftUndrawnDoesNotReturnForTheNextProcess)
{
    // A exits with the modal up: the pane is replaced, so the view is not drawn. ImGui itself closes
    // a modal that is not submitted (its next frame refocuses the window under it, which closes the
    // popups over that window), so this passes with or without the view's dismissal; it pins that the
    // stale modal cannot come back when B is selected and the Actions tab drawn again.
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Terminate, TARGET_A, "a");
    ASSERT_TRUE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "a", TARGET_A); }));
    EXPECT_TRUE(runFrame([] {})); // Not drawn, still open

    view.onSelectionChanged();
    EXPECT_FALSE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "b", TARGET_B); }));
    EXPECT_EQ(mock.terminateCount(), 0);
}

TEST_F(ProcessActionConfirmPopupTest, ViewClosesTheModalWhenTheLiveTargetMovesWithoutNotice)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Kill, TARGET_A, "a");
    ASSERT_TRUE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "a", TARGET_A); }));

    EXPECT_FALSE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "b", TARGET_B); }));
    EXPECT_FALSE(view.confirmRequested());
    EXPECT_EQ(view.pendingAction(), ProcessAction::None);
    EXPECT_EQ(mock.killCount(), 0);
}

TEST_F(ProcessActionConfirmPopupTest, ViewKeepsTheModalOpenForTheSameTarget)
{
    TestMocks::MockProcessActions mock;
    ProcessActionsView view;
    view.requestAction(ProcessAction::Stop, TARGET_A, "a");
    ASSERT_TRUE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "a", TARGET_A); }));
    EXPECT_TRUE(runFrame([&] { view.render(&mock, ALL_ACTIONS, "a", TARGET_A); }));
    EXPECT_TRUE(view.confirmRequested());
    EXPECT_EQ(mock.stopCount(), 0);
}

} // namespace
} // namespace App
