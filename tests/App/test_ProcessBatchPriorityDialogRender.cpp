/// @file test_ProcessBatchPriorityDialogRender.cpp
/// @brief The Processes table's batch priority flow, headless (#1484): the row menu's "Set priority for
/// N processes..." opens the dialog, which stays within the window; Cancel sets nothing; Continue
/// hands the picked value to the batch confirmation, whose "Set Priority" sets it once on every
/// target through the mock. The harness wires the pieces exactly as ProcessesPanel does.

#include "App/Panels/ProcessActionConfirm.h"
#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchPriorityDialog.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#ifdef _WIN32
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h" // The Windows class slider: its ID and minimum width
#include "App/Panels/ProcessPriorityView.h"                 // discretePrioritySliderMinWidth()
#endif

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): find and press a dialog's buttons

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace App
{
namespace
{

constexpr const char* DIALOG_POPUP_ID = "###BatchPriority";  // ProcessBatchPriorityDialog's modal
constexpr const char* CONFIRM_POPUP_ID = "###ConfirmAction"; // ProcessActionConfirm's modal
constexpr const char* ROW_MENU_ID = "RowMenu";
constexpr std::int32_t OWN_PID = 4000;

constexpr Platform::ProcessActionCapabilities CAN_SET_PRIORITY{.canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NO_PRIORITY{.canTerminate = true, .canKill = true, .canSetPriority = false};

class ProcessBatchPriorityDialogRenderTest : public ::testing::Test
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

    /// One frame of a window standing in for the Processes panel, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Processes", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Presses the top-most modal's button labelled @p label on the next frame.
    static void pressModalButton(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label, 0, modal->ID));
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

/// The row menu, the dialog and the confirmation, wired as ProcessesPanel wires them.
struct FlowHarness
{
    TestMocks::MockProcessActions mock;
    Platform::ProcessActionCapabilities capabilities = CAN_SET_PRIORITY;
    std::vector<ProcessBatch::BatchTarget> targets{
        {.target = {.pid = 10, .startTimeTicks = 100}, .name = "a"},
        {.target = {.pid = OWN_PID, .startTimeTicks = 400}, .name = "TaskSmack"},
        {.target = {.pid = 12, .startTimeTicks = 120}, .name = "c"},
    };
    ProcessBatchPriorityDialog dialog;
    bool openMenu = false;
    bool menuItemDrawn = false;
    ImGuiID menuItemId = 0;
    std::optional<std::int32_t> confirmNice;
    bool showConfirm = false;
    std::string title;
    std::string question;
    std::optional<Detail::ActionResultMessage> result;
    bool dialogOpen = false;
    bool confirmOpen = false;
    ImVec2 dialogPos;
    ImVec2 dialogSize;
    std::string dialogName;

    void frame()
    {
        if (openMenu)
        {
            ImGui::OpenPopup(ROW_MENU_ID);
            openMenu = false;
        }
        // ProcessesPanel::renderBatchPriorityDialog(), then renderRowActionConfirm()
        if (const std::optional<std::int32_t> nice = dialog.render(); nice.has_value())
        {
            confirmNice = nice;
            title = ProcessBatch::priorityConfirmTitle(targets.size());
            question = ProcessBatch::priorityConfirmBody(*nice, targets, OWN_PID);
            showConfirm = true;
        }
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
            modal != nullptr && std::string(modal->Name).contains(DIALOG_POPUP_ID))
        {
            dialogPos = modal->Pos;
            dialogSize = modal->Size;
            dialogName = modal->Name;
        }
        if (confirmNice.has_value())
        {
            const auto outcome =
                ProcessActionConfirm::renderLabelled(showConfirm, ProcessBatch::PRIORITY_CONFIRM_LABEL, false, title, question);
            if (outcome == ProcessActionConfirm::Outcome::Confirmed)
            {
                const ProcessBatch::BatchResult r = ProcessBatch::runBatchPriority(mock, targets, *confirmNice, OWN_PID);
                result = ProcessBatch::formatBatchPriorityResultMessage(*confirmNice, r);
                confirmNice.reset();
            }
            else if (outcome == ProcessActionConfirm::Outcome::Cancelled)
            {
                confirmNice.reset();
            }
        }
        // ProcessesPanel::renderRowContextMenu(), for a row of a three-process selection
        menuItemDrawn = false;
        if (ImGui::BeginPopup(ROW_MENU_ID))
        {
            if (ProcessBatch::offersBatchPriority(capabilities, targets.size()))
            {
                menuItemDrawn = true;
                if (ImGui::MenuItem("Set priority for 3 processes...###SetPriority"))
                {
                    dialog.open(targets.size());
                }
                menuItemId = ImGui::GetItemID();
            }
            ImGui::EndPopup();
        }
        dialogOpen = ImGui::IsPopupOpen(DIALOG_POPUP_ID);
        confirmOpen = ImGui::IsPopupOpen(CONFIRM_POPUP_ID);
    }

    /// Opens the row menu and picks its batch priority item.
    void chooseMenuItem(const std::function<void(const std::function<void()>&)>& run)
    {
        openMenu = true;
        run([this] { frame(); });
        ASSERT_TRUE(menuItemDrawn);
        ImGui::ActivateItemByID(menuItemId);
        run([this] { frame(); });
        run([this] { frame(); }); // The dialog opens on the frame after the click
    }
};

TEST_F(ProcessBatchPriorityDialogRenderTest, RowMenuItemOnlyWithTheCapability)
{
    FlowHarness flow;
    flow.capabilities = NO_PRIORITY;
    flow.openMenu = true;
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.menuItemDrawn);

    flow.capabilities = CAN_SET_PRIORITY;
    runFrame([&] { flow.frame(); });
    EXPECT_TRUE(flow.menuItemDrawn);
}

TEST_F(ProcessBatchPriorityDialogRenderTest, OpensFromTheRowMenuWithinTheWindow)
{
    // A narrow, short window: the dialog (and its slider) shrinks to fit rather than running off it.
    ImGui::GetIO().DisplaySize = ImVec2(500.0F, 400.0F);
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    runFrame([&] { flow.frame(); }); // Size settles
    EXPECT_TRUE(flow.dialogOpen);
    EXPECT_TRUE(flow.dialog.isPending());
    EXPECT_TRUE(flow.dialogName.starts_with("Set priority for 3 processes###"));
    EXPECT_EQ(flow.dialog.niceValue(), 0); // Starts at the default priority
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_GT(flow.dialogSize.x, 0.0F);
    EXPECT_GE(flow.dialogPos.x, 0.0F);
    EXPECT_GE(flow.dialogPos.y, 0.0F);
    EXPECT_LE(flow.dialogPos.x + flow.dialogSize.x, display.x);
    EXPECT_LE(flow.dialogPos.y + flow.dialogSize.y, display.y);
    EXPECT_EQ(flow.mock.setPriorityCount(), 0); // Opening it sets nothing
}

TEST_F(ProcessBatchPriorityDialogRenderTest, CancelSetsNothing)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    ASSERT_TRUE(flow.dialogOpen);
    flow.dialog.pickNice(10);

    pressModalButton("Cancel");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.dialogOpen);
    EXPECT_FALSE(flow.dialog.isPending());
    EXPECT_FALSE(flow.confirmOpen);
    EXPECT_FALSE(flow.confirmNice.has_value());
    EXPECT_EQ(flow.mock.setPriorityCount(), 0);
}

TEST_F(ProcessBatchPriorityDialogRenderTest, ContinueThenConfirmSetsThePickedValueOnceOnEveryTarget)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    ASSERT_TRUE(flow.dialogOpen);
    flow.dialog.pickNice(10);

    // Continue: the dialog closes and the batch confirmation opens; still nothing is set.
    pressModalButton("Continue");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.dialogOpen);
    ASSERT_TRUE(flow.confirmOpen);
    EXPECT_EQ(flow.confirmNice, 10);
    EXPECT_EQ(flow.title, "Set priority for 3 processes?");
    EXPECT_TRUE(flow.question.contains("TaskSmack (PID 4000)"));
    EXPECT_EQ(flow.mock.setPriorityCount(), 0);
    const ImGuiWindow* confirm = ImGui::GetTopMostPopupModal();
    ASSERT_NE(confirm, nullptr);
    EXPECT_TRUE(std::string(confirm->Name).starts_with("Set priority for 3 processes?###"));

    // Set Priority: each target once, by identity, with the picked value, TaskSmack itself last.
    pressModalButton(ProcessBatch::PRIORITY_CONFIRM_LABEL);
    runFrame([&] { flow.frame(); });
    for (int i = 0; i < 3; ++i)
    {
        runFrame([&] { flow.frame(); });
    }
    EXPECT_FALSE(flow.confirmOpen);
    EXPECT_EQ(flow.mock.setPriorityCount(), 3);
    EXPECT_EQ(flow.mock.lastSetPriorityNice(), 10);
    EXPECT_EQ(flow.mock.lastSetPriorityPid(), OWN_PID);
    EXPECT_EQ(flow.mock.lastTarget().startTimeTicks, 400U);
    const Detail::ActionResultMessage result = flow.result.value_or(Detail::ActionResultMessage{.ok = false, .text = "none"});
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.text, std::string("Priority set to ") + ProcessBatch::priorityValueText(10) + " for 3 processes");
}

#ifdef _WIN32
// The dialog picks with Process Details' own control: on Windows the slider with a stop per class
// (#1538), not a combo, wide enough for its five stop names.
TEST_F(ProcessBatchPriorityDialogRenderTest, OnWindowsTheDialogPicksWithTheClassSlider)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    runFrame([&] { flow.frame(); });
    ASSERT_TRUE(flow.dialogOpen);
    float sliderMinWidth = 0.0F;
    runFrame(
        [&]
        {
            sliderMinWidth = Detail::discretePrioritySliderMinWidth(Detail::WINDOWS_PRIORITY_SLIDER);
            flow.frame(); // An open popup not drawn for a frame closes
        });
    EXPECT_GE(flow.dialogSize.x, sliderMinWidth);

    ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    const ImGuiID sliderId = ImHashStr(Detail::WINDOWS_PRIORITY_SLIDER_ID, 0, modal->ID);
    bool focus = true;
    const auto body = [&]
    {
        if (focus)
        {
            ImGui::SetFocusID(sliderId, modal);
            focus = false;
        }
        flow.frame();
    };
    runFrame(body);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, true); // Normal to Below Normal
    runFrame(body);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_RightArrow, false);
    runFrame(body);
    EXPECT_EQ(flow.dialog.niceValue(), Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::BelowNormal));
    EXPECT_EQ(flow.mock.setPriorityCount(), 0);
}
#endif

TEST_F(ProcessBatchPriorityDialogRenderTest, CancellingTheConfirmationSetsNothing)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    pressModalButton("Continue");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    ASSERT_TRUE(flow.confirmOpen);

    pressModalButton("Cancel");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.confirmOpen);
    EXPECT_EQ(flow.mock.setPriorityCount(), 0);
    EXPECT_FALSE(flow.result.has_value());
}

} // namespace
} // namespace App
