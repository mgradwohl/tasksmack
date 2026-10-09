/// @file test_ProcessBatchPriorityDialogRender.cpp
/// @brief The Processes table's batch priority flow, headless (#1484, #1539): the row menu's "Set priority
/// for N processes..." opens one dialog, centred on the window and kept there through a resize, that
/// lists the processes with their current priority (capped, "+N more"); Cancel sets nothing; "Apply to
/// N processes" sets the picked value once on every listed target through the mock, with no second
/// confirmation dialog. The harness wires the pieces exactly as ProcessesPanel does.

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

#include <algorithm>
#include <cmath>
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

    /// Presses the top-most modal's filled button (UI::Widgets::filledButton(): "##filled" under the
    /// label's ID) labelled @p label on the next frame: the dialog's primary action.
    static void pressModalFilledButton(const char* label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr(label, 0, modal->ID)));
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

/// The row menu and the dialog, wired as ProcessesPanel wires them.
struct FlowHarness
{
    TestMocks::MockProcessActions mock;
    Platform::ProcessActionCapabilities capabilities = CAN_SET_PRIORITY;
    std::vector<ProcessBatch::BatchTarget> targets{
        {.target = {.pid = 10, .startTimeTicks = 100}, .name = "a", .priority = "Normal"},
        {.target = {.pid = OWN_PID, .startTimeTicks = 400}, .name = "TaskSmack", .priority = "Normal"},
        {.target = {.pid = 12, .startTimeTicks = 120}, .name = "c", .priority = "Below Normal"},
    };
    ProcessBatchPriorityDialog dialog;
    bool openMenu = false;
    bool menuItemDrawn = false;
    ImGuiID menuItemId = 0;
    std::optional<Detail::ActionResultMessage> result;
    bool dialogOpen = false;
    bool confirmEverOpened = false;
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
        // ProcessesPanel::renderBatchPriorityDialog(): Apply sets the value on the listed targets at once.
        if (const std::optional<std::int32_t> nice = dialog.render(); nice.has_value())
        {
            const ProcessBatch::BatchResult r = ProcessBatch::runBatchPriority(mock, dialog.targets(), *nice, OWN_PID);
            result = ProcessBatch::formatBatchPriorityResultMessage(*nice, r);
        }
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
            modal != nullptr && std::string(modal->Name).contains(DIALOG_POPUP_ID))
        {
            dialogPos = modal->Pos;
            dialogSize = modal->Size;
            dialogName = modal->Name;
        }
        // ProcessesPanel::renderRowContextMenu(), for a row of a selection
        menuItemDrawn = false;
        if (ImGui::BeginPopup(ROW_MENU_ID))
        {
            if (ProcessBatch::offersBatchPriority(capabilities, targets.size()))
            {
                menuItemDrawn = true;
                if (ImGui::MenuItem("Set priority for N processes...###SetPriority"))
                {
                    dialog.open(targets, OWN_PID); // requestSelectionPriority(): the resolved selection
                }
                menuItemId = ImGui::GetItemID();
            }
            ImGui::EndPopup();
        }
        dialogOpen = ImGui::IsPopupOpen(DIALOG_POPUP_ID);
        confirmEverOpened = confirmEverOpened || ImGui::IsPopupOpen(CONFIRM_POPUP_ID);
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
        run([this] { frame(); }); // ... and its size settles
    }

    /// How far the dialog's centre is from the main viewport's work centre, in pixels (the larger axis).
    [[nodiscard]] float offCentre() const
    {
        const ImVec2 centre = ImGui::GetMainViewport()->GetWorkCenter();
        const ImVec2 mid(dialogPos.x + (dialogSize.x * 0.5F), dialogPos.y + (dialogSize.y * 0.5F));
        return std::max(std::abs(mid.x - centre.x), std::abs(mid.y - centre.y));
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

TEST_F(ProcessBatchPriorityDialogRenderTest, OpensCentredOnTheWindowAndWithinIt)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    EXPECT_TRUE(flow.dialogOpen);
    EXPECT_TRUE(flow.dialog.isPending());
    EXPECT_TRUE(flow.dialogName.contains("Set priority for 3 processes###"));
    EXPECT_EQ(flow.dialog.niceValue(), 0); // Starts at the default priority
    EXPECT_LE(flow.offCentre(), 1.0F) << "dialog at " << flow.dialogPos.x << "," << flow.dialogPos.y;
    EXPECT_EQ(flow.mock.setPriorityCount(), 0); // Opening it sets nothing
}

TEST_F(ProcessBatchPriorityDialogRenderTest, StaysCentredAfterTheWindowIsResizedWhileOpen)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    ASSERT_LE(flow.offCentre(), 1.0F);

    ImGui::GetIO().DisplaySize = ImVec2(1200.0F, 800.0F);
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_TRUE(flow.dialogOpen);
    EXPECT_LE(flow.offCentre(), 1.0F) << "dialog at " << flow.dialogPos.x << "," << flow.dialogPos.y;
}

TEST_F(ProcessBatchPriorityDialogRenderTest, ANarrowShortWindowHoldsTheDialogWithinIt)
{
    // The dialog (and its slider) shrinks to fit rather than running off the window.
    ImGui::GetIO().DisplaySize = ImVec2(500.0F, 400.0F);
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    EXPECT_GT(flow.dialogSize.x, 0.0F);
    EXPECT_GE(flow.dialogPos.x, 0.0F);
    EXPECT_GE(flow.dialogPos.y, 0.0F);
    EXPECT_LE(flow.dialogPos.x + flow.dialogSize.x, display.x);
    EXPECT_LE(flow.dialogPos.y + flow.dialogSize.y, display.y);
}

TEST_F(ProcessBatchPriorityDialogRenderTest, ListsTheProcessesWithTheirPriorityTaskSmackFirst)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    const ProcessBatch::ListedTargets& listed = flow.dialog.listed();
    ASSERT_EQ(listed.listed.size(), 3U);
    EXPECT_EQ(listed.more, 0U);
    EXPECT_EQ(listed.listed[0]->name, "TaskSmack"); // Always named, first
    EXPECT_EQ(listed.listed[2]->priority, "Below Normal");
    EXPECT_EQ(flow.dialog.applyLabel(), "Apply to 3 processes");
}

TEST_F(ProcessBatchPriorityDialogRenderTest, TheListCapsAtItsLimitWithMore)
{
    FlowHarness flow;
    flow.targets.clear();
    for (int i = 0; i < 12; ++i)
    {
        flow.targets.push_back({.target = {.pid = 100 + i, .startTimeTicks = 1}, .name = "p", .priority = "Normal"});
    }
    flow.chooseMenuItem(runFrame);
    EXPECT_EQ(flow.dialog.listed().listed.size(), ProcessBatch::CONFIRM_LIST_LIMIT);
    EXPECT_EQ(flow.dialog.listed().more, 12U - ProcessBatch::CONFIRM_LIST_LIMIT);
    EXPECT_EQ(flow.dialog.applyLabel(), "Apply to 12 processes");
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
    EXPECT_FALSE(flow.confirmEverOpened);
    EXPECT_EQ(flow.mock.setPriorityCount(), 0);
    EXPECT_FALSE(flow.result.has_value());
}

TEST_F(ProcessBatchPriorityDialogRenderTest, ApplySetsThePickedValueOnceOnEveryTargetWithNoSecondDialog)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    ASSERT_TRUE(flow.dialogOpen);
    flow.dialog.pickNice(10);

    // Apply: each target once, by identity, with the picked value, TaskSmack itself last -- straight
    // away, the list in the dialog having been the confirmation (#1539).
    const std::string applyLabel = flow.dialog.applyLabel();
    pressModalFilledButton(applyLabel.c_str());
    for (int i = 0; i < 3; ++i)
    {
        runFrame([&] { flow.frame(); });
    }
    EXPECT_FALSE(flow.dialogOpen);
    EXPECT_FALSE(flow.confirmEverOpened); // One modal, not two
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

} // namespace
} // namespace App
