/// @file test_ProcessActionsBlockRender.cpp
/// @brief The Overview's Actions block (#1493), headless: one compact stack as wide as its widest row
/// -- the buttons at their labels' width, then the priority row(s) -- exactly as tall as the
/// Identity/Runtime row beside it and as tall as its content wrapped below it, held to a narrow pane,
/// and the shared confirm dialog still opens for it, acting on nothing until confirmed.

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

#include <algorithm>
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
            layout =
                ProcessDetailsLayout::computeActionsBlockLayout(paneWidth, infoRowWidth, ImGui::GetStyle().ItemSpacing.x, widths.content());
            const float height = rowChildHeight(infoRows);
            if (childHeight != nullptr)
            {
                *childHeight = height;
            }
            ProcessActionsBlock::render(h.context(capabilities), layout, height);
        };
        (void) runFrame(body);
        (void) runFrame(body);
        return layout;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessActionsBlockRenderTest, TheButtonsAreAsWideAsTheirLabelsNotAShareOfTheBlock)
{
    // The maintainer's #1511 review: Terminate and Kill were stretched to half the block each. They are
    // now equal at the wider label's natural width, side by side with the normal spacing.
    float buttons = 0.0F;
    float expected = 0.0F;
    float withoutPriority = 0.0F;
    float withIoPriorityOnly = 0.0F;
    (void) runFrame(
        [&]
        {
            buttons = ProcessActionsBlock::measure(NO_PRIORITY).buttons;
            const float widest = std::max(ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(0).label).x,
                                          ImGui::CalcTextSize(Detail::ACTION_BUTTONS.at(1).label).x);
            expected = ProcessDetailsLayout::computeActionButtonRowWidth(
                ProcessDetailsLayout::computeActionButtonWidth(widest, ImGui::GetStyle().FramePadding.x),
                2,
                ImGui::GetStyle().ItemSpacing.x);
            withoutPriority = ProcessActionsBlock::measure(NO_PRIORITY).priority;
            Platform::ProcessActionCapabilities ioOnly = NO_PRIORITY;
            ioOnly.canSetIoPriority = true;
            withIoPriorityOnly = ProcessActionsBlock::measure(ioOnly).priority;
        });
    EXPECT_FLOAT_EQ(buttons, expected);
    EXPECT_FLOAT_EQ(withoutPriority, 0.0F);
    // ProcessPriorityView also draws the I/O priority row on its own (#803), so it is measured.
    EXPECT_GT(withIoPriorityOnly, 0.0F);
}

TEST_F(ProcessActionsBlockRenderTest, BesideTheRowTheBlockIsTheRowsHeightAndItsContentsWidth)
{
    Harness h;
    float childHeight = 0.0F;
    const auto layout = renderBlock(h, ALL_ACTIONS, 1900.0F, 776.0F, 6.0F, &childHeight);
    ASSERT_TRUE(layout.besideInfo);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_FLOAT_EQ(block->Size.y, childHeight);
    EXPECT_NEAR(block->Size.x, layout.width, 1.0F); // ImGui rounds a window's size to whole pixels
    // Nothing in it is wider than the block allows: no stretched buttons, no row running out of it.
    EXPECT_FLOAT_EQ(block->ScrollMax.x, 0.0F);
}

#ifdef _WIN32
TEST_F(ProcessActionsBlockRenderTest, OnWindowsTheBlockIsTwoRowsAndFitsTheRowWithoutScrolling)
{
    // Windows' actions: [Terminate] [Kill], then Priority [class] [Apply] -- two framed rows, no
    // separator or header of the priority control's own -- well inside the six-row Identity/Runtime
    // row (Runtime always has its Type row on Windows).
    constexpr Platform::ProcessActionCapabilities WINDOWS_ACTIONS{
        .canTerminate = true, .canKill = true, .canStop = false, .canContinue = false, .canSetPriority = true};
    Harness h;
    (void) renderBlock(h, WINDOWS_ACTIONS, 1900.0F, 776.0F, 6.0F);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_FLOAT_EQ(block->ScrollMax.y, 0.0F);
    EXPECT_LE(block->ContentSize.y, (2.0F * ImGui::GetFrameHeightWithSpacing()) + 1.0F);
}
#endif

TEST_F(ProcessActionsBlockRenderTest, WrappedBelowTheRowTheBlockTakesItsContentHeight)
{
    Harness h;
    // A pane too narrow for a third block beside a 776px row.
    const auto layout = renderBlock(h, ALL_ACTIONS, 1000.0F, 776.0F, 0.0F);
    ASSERT_FALSE(layout.besideInfo);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    // Not the empty row's height it was handed: as tall as its content, with nothing to scroll.
    EXPECT_GT(block->Size.y, rowChildHeight(0.0F));
    EXPECT_FLOAT_EQ(block->ScrollMax.y, 0.0F);
    // Still its content's width, left-aligned, not the pane's.
    EXPECT_LT(block->Size.x, 1000.0F);
}

TEST_F(ProcessActionsBlockRenderTest, InANarrowPaneTheBlockIsHeldToItAndTheButtonsWrap)
{
    Harness h;
    const auto layout = renderBlock(h, NO_PRIORITY, 120.0F, 776.0F, 0.0F);
    ASSERT_FALSE(layout.besideInfo);

    const ImGuiWindow* block = blockWindow();
    ASSERT_NE(block, nullptr);
    EXPECT_LE(block->Size.x, 120.0F);
    // Kill went to a row of its own rather than being clipped off the right edge.
    EXPECT_FLOAT_EQ(block->ScrollMax.x, 0.0F);
    EXPECT_GT(block->ContentSize.y, ImGui::GetFrameHeightWithSpacing());
}

TEST_F(ProcessActionsBlockRenderTest, KillFromTheBlockOpensTheSharedConfirmAndActsOnNothingYet)
{
    Harness h;
    ASSERT_TRUE(h.actionsView.requestKillShortcut(ALL_ACTIONS, TARGET, h.name));

    std::string modalName;
    const auto body = [&]
    {
        const ProcessActionsBlock::Widths widths = ProcessActionsBlock::measure(ALL_ACTIONS);
        const auto layout =
            ProcessDetailsLayout::computeActionsBlockLayout(1600.0F, 600.0F, ImGui::GetStyle().ItemSpacing.x, widths.content());
        ProcessActionsBlock::render(h.context(ALL_ACTIONS), layout, rowChildHeight(6.0F));
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
        const auto layout =
            ProcessDetailsLayout::computeActionsBlockLayout(1000.0F, 0.0F, ImGui::GetStyle().ItemSpacing.x, widths.content());
        ProcessActionsBlock::render(h.context(ALL_ACTIONS), layout, rowChildHeight(6.0F));
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
