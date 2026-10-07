/// @file test_ProcessPriorityViewRender.cpp
/// @brief The priority control's real ImGui render, headless (#1179, slice 4): it draws nothing
/// without the capability, Apply is disabled until an edit, a click on Apply sets the edit on the
/// selected process only, an edit made for another process is dropped before it can be applied, and
/// the slider's keyboard shortcuts move the value as before. There is no popup or modal on Linux; the
/// Windows priority-class combo is not built here.

#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace App
{
namespace
{

constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};

constexpr Platform::ProcessActionCapabilities CAN_SET_PRIORITY{.canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NO_PRIORITY{
    .canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = false};

class ProcessPriorityViewRenderTest : public ::testing::Test
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

    /// One frame of a window like the Process Details pane, running @p body inside it.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(1600.0F, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Renders @p view for @p target and returns the centre of the Apply button, the last item it
    /// draws while there is no error line.
    static ImVec2 renderAndFindApply(ProcessPriorityView& view,
                                     TestMocks::MockProcessActions& mock,
                                     std::optional<std::int32_t> currentNice,
                                     const Platform::ProcessTarget& target)
    {
        ImVec2 centre;
        runFrame(
            [&]
            {
                view.render(&mock, CAN_SET_PRIORITY, currentNice, target);
                const ImVec2 min = ImGui::GetItemRectMin();
                const ImVec2 max = ImGui::GetItemRectMax();
                centre = ImVec2((min.x + max.x) * 0.5F, (min.y + max.y) * 0.5F);
            });
        return centre;
    }

    /// Clicks at @p pos over three frames (move, press, release) while @p body draws.
    static void click(ImVec2 pos, const std::function<void()>& body)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(pos.x, pos.y);
        runFrame(body);
        io.AddMouseButtonEvent(0, true);
        runFrame(body);
        io.AddMouseButtonEvent(0, false);
        runFrame(body);
    }

    /// Presses and releases @p key over two frames while @p body draws.
    static void pressKey(ImGuiKey key, const std::function<void()>& body)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        runFrame(body);
        io.AddKeyEvent(key, false);
        runFrame(body);
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessPriorityViewRenderTest, NothingIsDrawnWithoutTheCapability)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    float before = 0.0F;
    float after = 0.0F;
    runFrame(
        [&]
        {
            before = ImGui::GetCursorPosY();
            view.render(&mock, NO_PRIORITY, 0, TARGET_A);
            after = ImGui::GetCursorPosY();
        });
    EXPECT_FLOAT_EQ(before, after);
}

TEST_F(ProcessPriorityViewRenderTest, RenderShowsTheProcessValueUntilEdited)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    runFrame([&] { view.render(&mock, CAN_SET_PRIORITY, 7, TARGET_A); });
    EXPECT_EQ(view.niceValue(), 7);
    EXPECT_FALSE(view.hasPendingEdit());
}

TEST_F(ProcessPriorityViewRenderTest, ApplyIsDisabledUntilAnEdit)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindApply(view, mock, 0, TARGET_A);
    click(apply, [&] { view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A); });
    EXPECT_EQ(mock.setPriorityCount(), 0);
}

TEST_F(ProcessPriorityViewRenderTest, ClickingApplySetsTheEditOnTheSelectedProcessOnce)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindApply(view, mock, 0, TARGET_A);
    view.editNice(10, TARGET_A);

    click(apply, [&] { view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A); });
    ASSERT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(mock.lastSetPriorityNice(), 10);

    click(apply, [&] { view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A); }); // Disabled again
    EXPECT_EQ(mock.setPriorityCount(), 1);
}

TEST_F(ProcessPriorityViewRenderTest, AnEditForAnotherProcessIsDroppedBeforeApply)
{
    // Edited for A; the pane shows B without a selection change reaching the view. The render drops
    // the edit first, so Apply is disabled and the click reaches no platform call.
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindApply(view, mock, 0, TARGET_A);
    view.editNice(10, TARGET_A);

    click(apply, [&] { view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_B); });
    EXPECT_EQ(mock.setPriorityCount(), 0);
    EXPECT_FALSE(view.hasPendingEdit());
}

TEST_F(ProcessPriorityViewRenderTest, AFailedApplyShowsTheErrorLine)
{
    TestMocks::MockProcessActions mock;
    mock.setPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindApply(view, mock, 0, TARGET_A);
    view.editNice(-10, TARGET_A);

    click(apply, [&] { view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A); });
    EXPECT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(view.error(), "Permission denied");
    EXPECT_EQ(view.niceValue(), 0);
}

TEST_F(ProcessPriorityViewRenderTest, TheSliderKeysMoveTheValue)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    ImGuiID sliderId = 0;
    bool focus = true;
    const auto body = [&]
    {
        sliderId = ImGui::GetID("##priority_slider");
        if (focus)
        {
            // Keyboard focus on the slider, as a click or Tab would give it.
            ImGui::SetFocusID(sliderId, ImGui::GetCurrentWindow());
            focus = false;
        }
        view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A);
    };
    runFrame(body);
    runFrame(body);
    ASSERT_EQ(ImGui::GetCurrentContext()->NavId, sliderId);

    struct Step
    {
        ImGuiKey key;
        std::int32_t expected;
    };
    const std::array<Step, 8> steps{{
        {.key = ImGuiKey_End, .expected = 19},
        {.key = ImGuiKey_RightArrow, .expected = 19}, // Held at the end
        {.key = ImGuiKey_PageUp, .expected = 14},
        {.key = ImGuiKey_Home, .expected = -20},
        {.key = ImGuiKey_LeftArrow, .expected = -20},
        {.key = ImGuiKey_PageDown, .expected = -15},
        {.key = ImGuiKey_RightArrow, .expected = -14},
        {.key = ImGuiKey_0, .expected = 0},
    }};
    for (const Step& step : steps)
    {
        SCOPED_TRACE(ImGui::GetKeyName(step.key));
        pressKey(step.key, body);
        EXPECT_EQ(view.niceValue(), step.expected);
    }
    // As before the move out of the panel, an edit back to the process's own value stays pending.
    EXPECT_TRUE(view.hasPendingEdit());
}

} // namespace
} // namespace App
