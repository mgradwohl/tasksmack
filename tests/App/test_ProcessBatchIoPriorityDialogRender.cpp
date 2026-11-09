/// @file test_ProcessBatchIoPriorityDialogRender.cpp
/// @brief The Processes table's batch I/O priority flow, headless (#3551): the row menu's "Set I/O
/// priority for N processes..." opens one dialog, centred on the window, that lists the processes with
/// their current I/O priority; it starts at every process's default; Cancel sets nothing; "Apply to N
/// processes" sets the picked value once on every listed target through the mock, TaskSmack last. The
/// harness wires the pieces as ProcessesPanel does.

#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchIoPriorityDialog.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessIoPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IoPriorityTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GetTopMostPopupModal(), ImHashStr(), ActivateItemByID(): find and press a dialog's buttons

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace App
{
namespace
{

constexpr const char* DIALOG_POPUP_ID = "###BatchIoPriority"; // ProcessBatchIoPriorityDialog's modal
constexpr const char* ROW_MENU_ID = "RowMenu";
constexpr std::int32_t OWN_PID = 4000;

constexpr Platform::ProcessActionCapabilities CAN_SET_IO_PRIORITY{.canSetIoPriority = true};
constexpr Platform::ProcessActionCapabilities NO_IO_PRIORITY{.canSetPriority = true, .canSetIoPriority = false};
constexpr Platform::IoPriority IDLE{.ioClass = Platform::IoPriorityClass::Idle, .level = 0};

class ProcessBatchIoPriorityDialogRenderTest : public ::testing::Test
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
    Platform::ProcessActionCapabilities capabilities = CAN_SET_IO_PRIORITY;
    std::vector<ProcessBatch::BatchTarget> targets{
        {.target = {.pid = 10, .startTimeTicks = 100}, .name = "a", .priority = "best-effort 4"},
        {.target = {.pid = OWN_PID, .startTimeTicks = 400}, .name = "TaskSmack", .priority = "default (best-effort 4 from nice)"},
        {.target = {.pid = 12, .startTimeTicks = 120}, .name = "c", .priority = "idle"},
    };
    ProcessBatchIoPriorityDialog dialog;
    bool openMenu = false;
    bool menuItemDrawn = false;
    ImGuiID menuItemId = 0;
    std::optional<Detail::ActionResultMessage> result;
    bool dialogOpen = false;
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
        // ProcessesPanel::renderBatchIoPriorityDialog(): Apply sets the value on the listed targets.
        if (const std::optional<Platform::IoPriority> priority = dialog.render(); priority.has_value())
        {
            const ProcessBatch::BatchResult r = ProcessBatch::runBatchIoPriority(mock, dialog.targets(), *priority, OWN_PID);
            result = ProcessBatch::formatBatchIoPriorityResultMessage(*priority, r);
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
            if (ProcessBatch::offersBatchIoPriority(capabilities, targets.size()))
            {
                menuItemDrawn = true;
                if (ImGui::MenuItem("Set I/O priority for N processes...###SetIoPriority"))
                {
                    // requestSelectionIoPriority(): the resolved selection
                    dialog.open(targets, OWN_PID, capabilities.canSetRealtimeIoPriority, capabilities.ioPriorityHints);
                }
                menuItemId = ImGui::GetItemID();
            }
            ImGui::EndPopup();
        }
        dialogOpen = ImGui::IsPopupOpen(DIALOG_POPUP_ID);
    }

    /// Opens the row menu and picks its batch I/O priority item.
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

TEST_F(ProcessBatchIoPriorityDialogRenderTest, RowMenuItemOnlyWithTheCapability)
{
    FlowHarness flow;
    flow.capabilities = NO_IO_PRIORITY;
    flow.openMenu = true;
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.menuItemDrawn);

    flow.capabilities = CAN_SET_IO_PRIORITY;
    runFrame([&] { flow.frame(); });
    EXPECT_TRUE(flow.menuItemDrawn);
}

TEST_F(ProcessBatchIoPriorityDialogRenderTest, OpensCentredAtTheDefaultAndSetsNothing)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    EXPECT_TRUE(flow.dialogOpen);
    EXPECT_TRUE(flow.dialog.isPending());
    EXPECT_TRUE(flow.dialogName.contains("Set I/O priority for 3 processes###"));
    EXPECT_EQ(flow.dialog.value(), ProcessBatchIoPriorityDialog::defaultValue(false)); // The class never set
    EXPECT_EQ(flow.dialog.value().ioClass, Platform::IoPriorityClass::None);
    EXPECT_LE(flow.offCentre(), 1.0F) << "dialog at " << flow.dialogPos.x << "," << flow.dialogPos.y;
    EXPECT_EQ(flow.mock.setIoPriorityCount(), 0); // Opening it sets nothing
}

TEST_F(ProcessBatchIoPriorityDialogRenderTest, WindowsStartsAtTheNormalHint)
{
    FlowHarness flow;
    flow.capabilities.ioPriorityHints = true;
    flow.chooseMenuItem(runFrame);
    EXPECT_EQ(flow.dialog.value().ioClass, Platform::IoPriorityClass::WindowsHint);
    EXPECT_EQ(flow.dialog.value().level, static_cast<std::int32_t>(Platform::IoPriorityHint::Normal));
}

TEST_F(ProcessBatchIoPriorityDialogRenderTest, ListsTheProcessesWithTheirIoPriorityTaskSmackFirst)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    const ProcessBatch::ListedTargets& listed = flow.dialog.listed();
    ASSERT_EQ(listed.listed.size(), 3U);
    EXPECT_EQ(listed.more, 0U);
    EXPECT_EQ(listed.listed[0]->name, "TaskSmack"); // Always named, first
    EXPECT_EQ(listed.listed[2]->priority, "idle");
    EXPECT_EQ(flow.dialog.applyLabel(), "Apply to 3 processes");
}

TEST_F(ProcessBatchIoPriorityDialogRenderTest, CancelClosesItAndSetsNothing)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    flow.dialog.pick(IDLE);
    pressModalButton("Cancel");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.dialogOpen);
    EXPECT_FALSE(flow.dialog.isPending());
    EXPECT_EQ(flow.mock.setIoPriorityCount(), 0);
    EXPECT_FALSE(flow.result.has_value());
}

TEST_F(ProcessBatchIoPriorityDialogRenderTest, ApplySetsThePickedValueOnEveryListedProcess)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    flow.dialog.pick(IDLE);
    pressModalFilledButton("Apply to 3 processes");
    runFrame([&] { flow.frame(); });
    runFrame([&] { flow.frame(); });
    EXPECT_FALSE(flow.dialogOpen);
    EXPECT_EQ(flow.mock.setIoPriorityCount(), 3);
    EXPECT_EQ(flow.mock.lastSetIoPriority(), IDLE);
    EXPECT_EQ(flow.mock.setPriorityCount(), 0); // Never the nice value
    ASSERT_TRUE(flow.result.has_value());
    EXPECT_TRUE(flow.result->ok);
    EXPECT_EQ(flow.result->text, "I/O priority set to idle for 3 processes");
}

// #3566's review: the dialog shows the class never set hollow at best-effort 4 (nice 0's level), but each process
// inherits from its own nice value. Clicking that hollow stop must choose best-effort 4 for all of them, not leave
// the class unset, which would give a process at nice 10 best-effort 6.
TEST_F(ProcessBatchIoPriorityDialogRenderTest, ClickingTheHollowDefaultStopChoosesItExplicitly)
{
    FlowHarness flow;
    flow.chooseMenuItem(runFrame);
    ASSERT_EQ(flow.dialog.value().ioClass, Platform::IoPriorityClass::None);
    const auto body = [&]
    {
        flow.frame();
    };

    // Find the slider's track by hovering: a row of it, then its left and right ends.
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    const ImGuiID sliderId = ImHashStr(Detail::IO_PRIORITY_SLIDER_ID, 0, modal->ID);
    const ImVec2 origin = modal->Pos;
    const ImVec2 extent = modal->Size;
    const auto hoveredAt = [&](float x, float y)
    {
        ImGui::GetIO().AddMousePosEvent(x, y);
        runFrame(body);
        return ImGui::GetHoveredID() == sliderId;
    };
    const float probeX = origin.x + (extent.x * 0.5F);
    float row = -1.0F;
    for (float y = origin.y; y < origin.y + extent.y && row < 0.0F; y += 2.0F)
    {
        if (hoveredAt(probeX, y))
        {
            row = y;
        }
    }
    ASSERT_GE(row, 0.0F) << "the I/O slider was never hovered";
    float left = probeX;
    while (left > origin.x && hoveredAt(left - 1.0F, row))
    {
        left -= 1.0F;
    }
    float right = probeX;
    while (right < origin.x + extent.x && hoveredAt(right + 1.0F, row))
    {
        right += 1.0F;
    }
    ASSERT_GT(right - left, 30.0F);

    // Without Realtime the slider has nine stops, best-effort 0 to 7 then Idle: best-effort 4 is half way along.
    const ImVec2 stop(left + ((right - left) * 4.0F / 8.0F), row);
    ImGui::GetIO().AddMousePosEvent(stop.x, stop.y);
    runFrame(body);
    ImGui::GetIO().AddMouseButtonEvent(0, true);
    runFrame(body);
    ImGui::GetIO().AddMouseButtonEvent(0, false);
    runFrame(body);
    EXPECT_EQ(flow.dialog.value(), (Platform::IoPriority{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 4}))
        << "the click chose the stop it landed on";

    pressModalFilledButton("Apply to 3 processes");
    runFrame(body);
    runFrame(body);
    EXPECT_EQ(flow.mock.setIoPriorityCount(), 3);
    EXPECT_EQ(flow.mock.lastSetIoPriority(), (Platform::IoPriority{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 4}));
}

TEST(ProcessBatchIoPriorityDialogTest, APickIsNormalisedAsTheSliderWouldMakeIt)
{
    ProcessBatchIoPriorityDialog dialog;
    dialog.pick({.ioClass = Platform::IoPriorityClass::Idle, .level = 5}); // Idle has no levels
    EXPECT_EQ(dialog.value(), IDLE);
    dialog.pick({.ioClass = Platform::IoPriorityClass::BestEffort, .level = 3});
    EXPECT_EQ(dialog.value().level, 3);
}

// The list points into the dialog's own targets, so a copy would point into the source's (#2143).
static_assert(!std::is_copy_constructible_v<ProcessBatchIoPriorityDialog>);
static_assert(!std::is_copy_assignable_v<ProcessBatchIoPriorityDialog>);
static_assert(std::is_nothrow_move_constructible_v<ProcessBatchIoPriorityDialog>);
static_assert(std::is_nothrow_move_assignable_v<ProcessBatchIoPriorityDialog>);

} // namespace
} // namespace App
