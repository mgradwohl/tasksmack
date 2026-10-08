/// @file test_ProcessActionsBlockRender.cpp
/// @brief The Overview's Actions block (#1493), headless: beside Identity and Runtime it is exactly as
/// tall as their row, wrapped below them it is as tall as its content, side by side it is shorter than
/// stacked, and the shared confirm dialog still opens from it, acting on nothing until confirmed.

#include "App/Panels/ProcessActionsBlock.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessDetailsLayout.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // The block's child window, for its size, and the open modal

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 1001, .startTimeTicks = 5000};

constexpr Platform::ProcessActionCapabilities ALL_ACTIONS{
    .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NO_PRIORITY{
    .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = false};

class ProcessActionsBlockRenderTest : public ::testing::Test
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
    /// any popup (the confirm modal) is open at the end of it.
    static bool runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        const bool open = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        ImGui::End();
        ImGui::Render();
        return open;
    }

    /// The block's child window, as the last frame left it.
    [[nodiscard]] static const ImGuiWindow* blockWindow()
    {
        for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
        {
            if (std::string_view{window->Name}.contains("ProcessActionsBlock_"))
            {
                return window;
            }
        }
        return nullptr;
    }

    /// The Identity/Runtime child height for @p rows rows, worked out as ProcessDetailsPanel does.
    [[nodiscard]] static float rowChildHeight(float rows)
    {
        return (ImGui::GetTextLineHeightWithSpacing() * rows) + (ImGui::GetStyle().WindowPadding.y * 2.0F);
    }

    struct Harness
    {
        TestMocks::MockProcessActions mock;
        ProcessActionsView actionsView;
        ProcessPriorityView priorityView;
        std::string name = "victim";

        [[nodiscard]] ProcessActionsBlock::Context context(const Platform::ProcessActionCapabilities& capabilities)
        {
            return ProcessActionsBlock::Context{
                .actionsView = &actionsView,
                .priorityView = &priorityView,
                .actions = &mock,
                .capabilities = capabilities,
                .processName = &name,
                .target = TARGET,
                .currentNice = std::optional<std::int32_t>{0},
            };
        }
    };

    /// Draws the block for @p capabilities, laid out as the panel lays it out in @p paneWidth beside an
    /// Identity/Runtime row @p infoRowWidth wide whose children hold @p infoRows rows (rowChildHeight(),
    /// worked out in the frame, where the font is known; its value is left in @p childHeight); two frames, so
    /// an auto-resizing child has settled.
    static ProcessDetailsLayout::ActionsBlockLayout renderBlock(Harness& h,
                                                                const Platform::ProcessActionCapabilities& capabilities,
                                                                float paneWidth,
                                                                float infoRowWidth,
                                                                float infoRows,
                                                                float* childHeight = nullptr)
    {
        ProcessDetailsLayout::ActionsBlockLayout layout;
        const auto body = [&]
        {
            const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(capabilities);
            layout = ProcessDetailsLayout::computeActionsBlockLayout(paneWidth,
                                                                     infoRowWidth,
                                                                     ImGui::GetStyle().ItemSpacing.x,
                                                                     widths.controls,
                                                                     widths.priority,
                                                                     widths.columnGap,
                                                                     widths.padding);
            const float height = rowChildHeight(infoRows);
            if (childHeight != nullptr)
            {
                *childHeight = height;
            }
            ProcessActionsBlock::render(h.context(capabilities), widths, layout, height);
        };
        (void) runFrame(body);
        (void) runFrame(body);
        return layout;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessActionsBlockRenderTest, MeasureLeavesNoPriorityColumnWithoutTheCapability)
{
    float withPriority = 0.0F;
    float withoutPriority = 0.0F;
    float withIoPriorityOnly = 0.0F;
    float controls = 0.0F;
    float emPx = 0.0F;
    (void) runFrame(
        [&]
        {
            emPx = ImGui::GetFontSize();
            withPriority = ProcessActionsBlock::measure(ALL_ACTIONS).priority;
            withoutPriority = ProcessActionsBlock::measure(NO_PRIORITY).priority;
            Platform::ProcessActionCapabilities ioOnly = NO_PRIORITY;
            ioOnly.canSetIoPriority = true;
            withIoPriorityOnly = ProcessActionsBlock::measure(ioOnly).priority;
            controls = ProcessActionsBlock::measure(ALL_ACTIONS).controls;
        });
    EXPECT_FLOAT_EQ(withPriority, ProcessDetailsLayout::ACTIONS_PRIORITY_COLUMN_WIDTH_EM * emPx);
    EXPECT_FLOAT_EQ(withoutPriority, 0.0F);
    // ProcessPriorityView also draws the I/O priority control on its own (#803), so it gets the column.
    EXPECT_FLOAT_EQ(withIoPriorityOnly, withPriority);
    // Two buttons at least their em floor each.
    EXPECT_GE(controls, 2.0F * ProcessDetailsLayout::ACTION_BUTTON_MIN_WIDTH_EM * emPx);
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheRowTheBlockIsExactlyTheRowsHeight)
{
    Harness h;
    float childHeight = 0.0F;
    const auto layout = renderBlock(h, ALL_ACTIONS, 1600.0F, 600.0F, 6.0F, &childHeight);
    ASSERT_TRUE(layout.besideInfo);
    ASSERT_TRUE(layout.columnsSideBySide);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_FLOAT_EQ(block->Size.y, childHeight);
    EXPECT_NEAR(block->Size.x, layout.width, 1.0F); // ImGui rounds a window's size to whole pixels
}

#ifdef _WIN32
TEST_F(ProcessActionsBlockRenderTest, OnWindowsTheBlockFitsTheRowWithoutScrolling)
{
    // Windows' actions -- Terminate, Kill and the priority-class combo -- side by side fit in the
    // Identity/Runtime row, so the block shows whole beside it. The row is six rows tall on Windows:
    // Runtime always has its Type row there. (Linux's priority slider, with its value badge, is taller
    // and may scroll.)
    constexpr Platform::ProcessActionCapabilities WINDOWS_ACTIONS{
        .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = true};
    Harness h;
    (void) renderBlock(h, WINDOWS_ACTIONS, 1600.0F, 600.0F, 6.0F);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_FLOAT_EQ(block->ScrollMax.y, 0.0F);
}
#endif

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheBlockTakesItsContentHeight)
{
    Harness h;
    // A pane too narrow for a third block beside a 600px row, but wide enough for its parts side by side.
    const auto layout = renderBlock(h, ALL_ACTIONS, 1000.0F, 600.0F, 0.0F);
    ASSERT_FALSE(layout.besideInfo);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    // Not the empty row's height it was handed: as tall as its content, with nothing to scroll.
    EXPECT_GT(block->Size.y, rowChildHeight(0.0F));
    EXPECT_FLOAT_EQ(block->ScrollMax.y, 0.0F);
}

TEST_F(ProcessActionsBlockRenderTest, SideBySideIsShorterThanStacked)
{
    // The reason the block can sit beside Identity and Runtime at all: its two parts side by side
    // need less height than one above the other.
    Harness wide;
    const auto sideBySide = renderBlock(wide, ALL_ACTIONS, 1000.0F, 600.0F, 0.0F);
    ASSERT_TRUE(sideBySide.columnsSideBySide);
    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    const float sideBySideHeight = block->ContentSize.y;

    Harness narrow;
    const auto stacked = renderBlock(narrow, ALL_ACTIONS, 420.0F, 600.0F, 0.0F);
    ASSERT_FALSE(stacked.columnsSideBySide);
    block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_GT(block->ContentSize.y, sideBySideHeight);
    // Held to the pane, so nothing in it is out of reach to the right.
    EXPECT_LE(block->Size.x, 420.0F);
}

TEST_F(ProcessActionsBlockRenderTest, KillFromTheBlockOpensTheSharedConfirmAndActsOnNothingYet)
{
    Harness h;
    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name));

    std::string modalName;
    const auto body = [&]
    {
        const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(ALL_ACTIONS);
        const auto layout = ProcessDetailsLayout::computeActionsBlockLayout(
            1600.0F, 600.0F, ImGui::GetStyle().ItemSpacing.x, widths.controls, widths.priority, widths.columnGap, widths.padding);
        ProcessActionsBlock::render(h.context(ALL_ACTIONS), widths, layout, rowChildHeight(6.0F));
        // As ProcessDetailsPanel does: the dialog from panel scope, every frame.
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalName = modal->Name;
        }
    };
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(modalName.starts_with("Kill")) << modalName;
    EXPECT_EQ(h.actionsView.pendingAction(), Detail::ProcessAction::Kill);
    EXPECT_EQ(h.mock.killCount(), 0);

    // A selection change still closes it unconfirmed.
    h.actionsView.onSelectionChanged();
    EXPECT_FALSE(runFrame(body));
    EXPECT_EQ(h.mock.killCount(), 0);
}

TEST_F(ProcessActionsBlockRenderTest, F9OpensTheKillConfirmWhileTheBlockIsScrolledOutOfView)
{
    // The Overview scrolled so the Actions block is out of view: ImGui skips the block's child, and
    // everything in it. The confirm dialog is submitted from outside it, so F9's Kill confirm still
    // opens (Copilot review on #1511) instead of staying pending with no dialog and refusing every
    // later F9.
    Harness h;
    bool blockSkipped = false;
    std::string modalName;
    const auto body = [&]
    {
        // A short scrolling Overview with the block far below its visible part.
        ImGui::BeginChild("##OverviewContent", ImVec2(1000.0F, 100.0F));
        ImGui::Dummy(ImVec2(10.0F, 2000.0F));
        const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(ALL_ACTIONS);
        const auto layout = ProcessDetailsLayout::computeActionsBlockLayout(
            1000.0F, 0.0F, ImGui::GetStyle().ItemSpacing.x, widths.controls, widths.priority, widths.columnGap, widths.padding);
        ProcessActionsBlock::render(h.context(ALL_ACTIONS), widths, layout, rowChildHeight(6.0F));
        const ImGuiWindow* block = blockWindow();
        blockSkipped = block == nullptr || block->SkipItems;
        ImGui::EndChild();
        h.actionsView.renderConfirmation(&h.mock, TARGET);
        if (const ImGuiWindow* modal = ImGui::GetTopMostPopupModal(); modal != nullptr)
        {
            modalName = modal->Name;
        }
    };
    (void) runFrame(body); // Lays the Overview out, so the next frames know the block is clipped
    ASSERT_FALSE(runFrame(body));

    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name)); // F9
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(runFrame(body));
    EXPECT_TRUE(blockSkipped) << "the block was drawn, so this does not test the scrolled-out case";
    EXPECT_TRUE(modalName.starts_with("Kill")) << modalName;
    EXPECT_EQ(h.mock.killCount(), 0);
}

} // namespace
} // namespace App
