/// @file test_ProcessActionConfirmPopup.cpp
/// @brief The confirm modal's real ImGui lifecycle, headless (Copilot review on #1447): clearing the
/// request flag alone leaves an open modal open, `dismiss` closes it, and ProcessActionsView closes
/// it unconfirmed after a selection change, or when the live target moved, so a confirm can never act
/// on a process other than the one it was requested for.

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(): the open modal's window, for its title and width

#include <functional>
#include <string>
#include <vector>

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

    /// Draws the Actions block's controls with @p capabilities, then clicks where its last item -- the trace button
    /// (#182), when drawn -- was. Returns how many times the mock was asked to trace.
    static int clickTraceButton(const Platform::ProcessActionCapabilities& capabilities,
                                const Platform::ProcessTarget& target,
                                float* drawnWidth = nullptr)
    {
        TestMocks::MockProcessActions mock;
        ProcessActionsView view;
        ImVec2 center(-1.0F, -1.0F);
        const auto draw = [&]
        {
            view.render(&mock, capabilities, "a", target);
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            center = ImVec2((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F);
        };
        float naturalWidth = 0.0F;
        static_cast<void>(runFrame(
            [&]
            {
                draw();
                if (drawnWidth != nullptr)
                {
                    *drawnWidth = ImGui::GetItemRectSize().x;
                }
                naturalWidth = ProcessActionsView::syscallTraceButtonWidth(capabilities);
            }));
        if (drawnWidth != nullptr)
        {
            // Report it relative to the label's natural width: 1 means not stretched.
            *drawnWidth = naturalWidth > 0.0F ? *drawnWidth / naturalWidth : 0.0F;
        }
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(center.x, center.y);
        static_cast<void>(runFrame(draw));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(runFrame(draw));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(runFrame(draw));
        static_cast<void>(runFrame(draw));
        return mock.syscallTraceCount();
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

TEST_F(ProcessActionConfirmPopupTest, BatchConfirmUsesTheSameModalAndStaysWithinTheWindow)
{
    // The Processes table's batch confirm (#804): the caller's title and multi-line question, in the
    // same "###ConfirmAction" modal, no wider than the dialog budget however long a listed name is.
    std::vector<ProcessBatch::BatchTarget> targets;
    targets.push_back({.target = TARGET_A, .name = std::string(400, 'x')});
    targets.push_back({.target = TARGET_B, .name = "b"});
    const std::string title = ProcessBatch::confirmTitle(ProcessAction::Kill, targets.size());
    const std::string question = ProcessBatch::confirmBody(ProcessAction::Kill, targets, 0);

    bool show = true;
    float modalWidth = 0.0F;
    std::string modalName;
    const auto body = [&]
    {
        EXPECT_EQ(ProcessActionConfirm::renderText(show, ProcessAction::Kill, title, question), ProcessActionConfirm::Outcome::None);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalWidth = modal->Size.x;
            modalName = modal->Name;
        }
    };
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body)); // Auto-fit settles on the second frame
    EXPECT_TRUE(modalName.starts_with("Kill 2 processes?###"));
    EXPECT_GT(modalWidth, 0.0F);
    EXPECT_LE(modalWidth, ImGui::GetIO().DisplaySize.x);

    auto outcome = ProcessActionConfirm::Outcome::None;
    EXPECT_FALSE(runFrame([&] { outcome = ProcessActionConfirm::renderText(show, ProcessAction::Kill, title, question, true); }));
    EXPECT_EQ(outcome, ProcessActionConfirm::Outcome::Cancelled);
    EXPECT_FALSE(show);
}

TEST_F(ProcessActionConfirmPopupTest, ATallBatchConfirmScrollsAndKeepsItsButtonsInAShortWindow)
{
    // A short window and a question longer than it (long names wrap over many lines): the dialog
    // stays inside the window, so its buttons can still be reached (#804 review).
    ImGui::GetIO().DisplaySize = ImVec2(700.0F, 240.0F);
    constexpr int TARGET_COUNT = 12;
    std::vector<ProcessBatch::BatchTarget> targets;
    targets.reserve(TARGET_COUNT);
    for (int i = 0; i < TARGET_COUNT; ++i)
    {
        targets.push_back({.target = {.pid = 100 + i, .startTimeTicks = 1}, .name = std::string(300, static_cast<char>('a' + i))});
    }
    const std::string title = ProcessBatch::confirmTitle(ProcessAction::Kill, targets.size());
    const std::string question = ProcessBatch::confirmBody(ProcessAction::Kill, targets, 0);

    bool show = true;
    ImVec2 modalPos{};
    ImVec2 modalSize{};
    float modalScrollMax = -1.0F;
    const auto body = [&]
    {
        static_cast<void>(ProcessActionConfirm::renderText(show, ProcessAction::Kill, title, question));
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalPos = modal->Pos;
            modalSize = modal->Size;
            modalScrollMax = modal->ScrollMax.y;
        }
    };
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_GT(modalSize.y, 0.0F);
    EXPECT_LE(modalSize.y, ImGui::GetIO().DisplaySize.y);
    EXPECT_GE(modalPos.y, 0.0F);
    EXPECT_LE(modalPos.y + modalSize.y, ImGui::GetIO().DisplaySize.y);
    // ImGui clamps an auto-fitting window to the display on its own; what matters is that nothing in
    // it -- the footer's buttons above all -- lies below what it shows: the dialog itself never needs
    // scrolling, only the question does.
    EXPECT_FLOAT_EQ(modalScrollMax, 0.0F);
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
    // stale modal cannot come back when B is selected and the Actions block drawn again.
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

// --- Trace system calls (#182) ---------------------------------------------------------------------

TEST_F(ProcessActionConfirmPopupTest, AvailableTraceButtonLaunchesOnClickWithoutAConfirm)
{
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.syscallTrace = Platform::SyscallTraceAvailability::Available;
    float widthRatio = 0.0F;
    EXPECT_EQ(clickTraceButton(caps, TARGET_A, &widthRatio), 1);
    // Drawn at its own label's width after the row, not stretched to the other buttons' or the pane's.
    EXPECT_NEAR(widthRatio, 1.0F, 0.01F);
}

TEST_F(ProcessActionConfirmPopupTest, DisabledTraceButtonDoesNothingWhenClicked)
{
    for (const auto availability : {Platform::SyscallTraceAvailability::NoTracer, Platform::SyscallTraceAvailability::NoTerminal})
    {
        SCOPED_TRACE(static_cast<int>(availability));
        Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
        caps.syscallTrace = availability;
        EXPECT_EQ(clickTraceButton(caps, TARGET_A), 0);
    }
    Platform::ProcessActionCapabilities caps = ALL_ACTIONS;
    caps.syscallTrace = Platform::SyscallTraceAvailability::Available;
    EXPECT_EQ(clickTraceButton(caps, {.pid = 0, .startTimeTicks = 0}), 0);
}

} // namespace
} // namespace App
