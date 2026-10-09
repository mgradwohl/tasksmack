/// @file test_ProcessPriorityViewRender.cpp
/// @brief The priority control's real ImGui render, headless (#1179, slice 4): it draws nothing
/// without the capability, Apply is disabled until an edit, a click on Apply sets the edit on the
/// selected process only, an edit made for another process is dropped before it can be applied, and
/// the slider's keyboard shortcuts move the value as before (Linux only: the slider is the Linux control).
/// There is no popup or modal on Linux. On Windows the same slider has a stop per priority class (#1538).

#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#ifdef _WIN32
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h" // The class slider's ID
#include "Domain/PriorityConfig.h"                          // MIN_NICE: what the probe reports for Realtime
#endif

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // SetFocusID(): keyboard focus on the slider

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

    /// Presses and releases @p key over two frames while @p body draws (the slider's keys).
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

#ifdef _WIN32
// Windows' control is the slider with a stop per priority class (#1538), not a combo: the keys step
// through the classes, held at the ends, and back on the process's own class there is no edit.
TEST_F(ProcessPriorityViewRenderTest, TheClassSliderKeysStepThroughTheClasses)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    ImGuiID sliderId = 0;
    bool focus = true;
    const auto body = [&]
    {
        sliderId = ImGui::GetID(Detail::WINDOWS_PRIORITY_SLIDER_ID);
        if (focus)
        {
            ImGui::SetFocusID(sliderId, ImGui::GetCurrentWindow());
            focus = false;
        }
        view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A); // Normal
    };
    runFrame(body);
    runFrame(body);
    ASSERT_EQ(ImGui::GetCurrentContext()->NavId, sliderId);

    struct Step
    {
        ImGuiKey key;
        std::int32_t expected;
    };
    const std::array<Step, 10> steps{{
        {.key = ImGuiKey_RightArrow, .expected = 10}, // Below Normal
        {.key = ImGuiKey_DownArrow, .expected = 19},  // Idle
        {.key = ImGuiKey_RightArrow, .expected = 19}, // Held at the low end
        {.key = ImGuiKey_Home, .expected = -15},      // High
        {.key = ImGuiKey_LeftArrow, .expected = -15}, // Held at the high end: never Realtime
        {.key = ImGuiKey_DownArrow, .expected = -7},  // Above Normal
        {.key = ImGuiKey_UpArrow, .expected = -15},
        {.key = ImGuiKey_End, .expected = 19},
        {.key = ImGuiKey_LeftArrow, .expected = 10},
        {.key = ImGuiKey_LeftArrow, .expected = 0}, // Normal: the process's own class
    }};
    for (const Step& step : steps)
    {
        SCOPED_TRACE(ImGui::GetKeyName(step.key));
        pressKey(step.key, body);
        EXPECT_EQ(view.niceValue(), step.expected);
        EXPECT_EQ(view.hasPendingEdit(), step.expected != 0); // Apply only for another class
    }
}

TEST_F(ProcessPriorityViewRenderTest, ARealtimeProcessShowsRealtimeAndCanOnlyBeLowered)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    constexpr std::int32_t REALTIME = Domain::Priority::MIN_NICE;
    ImGuiID sliderId = 0;
    bool focus = true;
    const auto body = [&]
    {
        sliderId = ImGui::GetID(Detail::WINDOWS_PRIORITY_SLIDER_ID);
        if (focus)
        {
            ImGui::SetFocusID(sliderId, ImGui::GetCurrentWindow());
            focus = false;
        }
        view.render(&mock, CAN_SET_PRIORITY, REALTIME, TARGET_A);
    };
    runFrame(body);
    runFrame(body);
    EXPECT_EQ(view.niceValue(), REALTIME);

    pressKey(ImGuiKey_LeftArrow, body); // Toward Realtime: nothing to pick
    pressKey(ImGuiKey_Home, body);
    EXPECT_EQ(view.niceValue(), REALTIME);
    EXPECT_FALSE(view.hasPendingEdit());

    pressKey(ImGuiKey_RightArrow, body); // Off Realtime onto High
    EXPECT_EQ(view.niceValue(), -15);
    EXPECT_TRUE(view.hasPendingEdit());
    EXPECT_EQ(mock.setPriorityCount(), 0); // Picking applies nothing
}
#else
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

// In a pane narrower than the slider's authored width (the Overview Actions block is sized to its
// content, #1511) the track is shortened until "Low" ends flush with the content edge. A container
// that pushes a wrap position for its own lines used to split it into "Lo" / "w" once rounding put
// that edge a fraction short (#1560). The scale labels must ignore an inherited wrap position:
// pushed just short of the edge, the control is exactly as tall as without one.
TEST_F(ProcessPriorityViewRenderTest, TheScaleLabelsIgnoreAnInheritedWrapPosition)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const auto heightOf = [&](bool pushWrap)
    {
        float height = 0.0F;
        const auto body = [&]
        {
            // Narrow enough that the track fills it and "Low" ends at the edge.
            if (ImGui::BeginChild("Narrow", ImVec2(260.0F, 400.0F)))
            {
                const float top = ImGui::GetCursorPosY();
                if (pushWrap)
                {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2.0F);
                }
                view.render(&mock, CAN_SET_PRIORITY, std::optional<std::int32_t>{0}, TARGET_A);
                if (pushWrap)
                {
                    ImGui::PopTextWrapPos();
                }
                height = ImGui::GetCursorPosY() - top;
            }
            ImGui::EndChild();
        };
        runFrame(body);
        runFrame(body);
        return height;
    };

    const float unwrapped = heightOf(false);
    ASSERT_GT(unwrapped, 0.0F);
    EXPECT_FLOAT_EQ(heightOf(true), unwrapped);
}
#endif

#ifdef _WIN32
// The Windows class slider (#1204, #1538; #1566's coverage): the class picked on it is what Apply
// sets, and a Realtime class set outside TaskSmack is shown with a warning, never offered.

TEST_F(ProcessPriorityViewRenderTest, PickingAClassOnTheSliderAndApplyingSetsThatClass)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const std::int32_t normal = Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::Normal);
    const ImVec2 apply = renderAndFindApply(view, mock, normal, TARGET_A);

    bool focus = true;
    const auto body = [&]
    {
        if (focus)
        {
            ImGui::SetFocusID(ImGui::GetID(Detail::WINDOWS_PRIORITY_SLIDER_ID), ImGui::GetCurrentWindow());
            focus = false;
        }
        view.render(&mock, CAN_SET_PRIORITY, normal, TARGET_A);
    };
    runFrame(body);
    pressKey(ImGuiKey_Home, body); // High, the high-priority end
    const std::int32_t high = Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::High);
    EXPECT_EQ(view.niceValue(), high);
    EXPECT_TRUE(view.hasPendingEdit());
    EXPECT_EQ(mock.setPriorityCount(), 0); // Picking alone sends nothing

    click(apply, body);
    ASSERT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(mock.lastSetPriorityNice(), high);
    EXPECT_EQ(Detail::windowsPriorityClassFromNice(mock.lastSetPriorityNice()), Detail::WindowsPriorityClass::High);
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
}

TEST_F(ProcessPriorityViewRenderTest, ARealtimeProcessGetsAWarningLine)
{
    TestMocks::MockProcessActions mock;
    const auto heightFor = [&](std::int32_t currentNice)
    {
        ProcessPriorityView view;
        float height = 0.0F;
        runFrame(
            [&]
            {
                const float top = ImGui::GetCursorPosY();
                view.render(&mock, CAN_SET_PRIORITY, currentNice, TARGET_A);
                height = ImGui::GetCursorPosY() - top;
            });
        EXPECT_EQ(view.niceValue(), currentNice);
        return height;
    };

    const float normal = heightFor(Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::Normal));
    const float realtime = heightFor(Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::Realtime));
    // One more line: the warning that Realtime was set elsewhere and can only be lowered here.
    EXPECT_GT(realtime, normal);
    EXPECT_EQ(mock.setPriorityCount(), 0);
}
#endif

} // namespace
} // namespace App
