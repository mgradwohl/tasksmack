/// @file test_ProcessPriorityViewRender.cpp
/// @brief The priority control's real ImGui render, headless (#1179, slice 4): it draws nothing
/// without the capability, Apply is disabled until an edit, a click on Apply sets the edit on the
/// selected process only, an edit made for another process is dropped before it can be applied, and
/// the slider's keyboard shortcuts move the value as before (Linux only: the slider is the Linux control).
/// There is no popup or modal on Linux. On Windows, the priority-class combo: the class picked from
/// its popup is what Apply sets, and a Realtime class gets a warning line (#1566). On both, Apply is as
/// wide as its label and the row repeats no "current" value (#1537).

#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // The frame's text log, the window's draw list, the combo's popup

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#ifdef _WIN32
#include "App/Panels/ProcessDetailsPanel_PriorityHelpers.h"

#include <cstddef>
#else
#include <array>
#endif

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

#ifndef _WIN32
    /// Presses and releases @p key over two frames while @p body draws (the slider's keys).
    static void pressKey(ImGuiKey key, const std::function<void()>& body)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(key, true);
        runFrame(body);
        io.AddKeyEvent(key, false);
        runFrame(body);
    }
#endif

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

// The maintainer's #1511 check: Apply was an em floor wide, wider than the class combo beside it and
// a large muted bar while disabled. It is as wide as its label, as Terminate and Kill are (#1537).
TEST_F(ProcessPriorityViewRenderTest, ApplyIsAsWideAsItsLabel)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    float applyWidth = 0.0F;
    float labelWidth = 0.0F;
    runFrame(
        [&]
        {
            view.render(&mock, CAN_SET_PRIORITY, 0, TARGET_A);
            applyWidth = ImGui::GetItemRectSize().x; // Apply is the last item while there is no error line
            labelWidth = ImGui::CalcTextSize(ICON_FA_CHECK "  Apply").x + (2.0F * ImGui::GetStyle().FramePadding.x);
        });
    EXPECT_GT(applyWidth, 0.0F);
    EXPECT_LE(applyWidth, std::ceil(labelWidth));
}

// Runtime's Priority row shows the current priority ("Normal (nice: 0)" on Linux, the class on
// Windows), and the combo or slider opens on it, so the row does not repeat it (#1537): no
// "current: <class>" after Apply on Windows, no "current nice: N" beside the label on Linux.
TEST_F(ProcessPriorityViewRenderTest, TheRowRepeatsNoCurrentValue)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    std::string logged;
    ImVec2 applyMin;
    ImVec2 applyMax;
    const ImDrawList* drawList = nullptr;
    runFrame(
        [&]
        {
            ImGui::LogToBuffer();
            view.render(&mock, CAN_SET_PRIORITY, 7, TARGET_A);
            applyMin = ImGui::GetItemRectMin();
            applyMax = ImGui::GetItemRectMax();
            logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
            ImGui::LogFinish();
            drawList = ImGui::GetWindowDrawList();
        });
    // Text drawn as an item (Linux's label line) is in the frame's log.
    EXPECT_TRUE(logged.contains("Priority")) << logged;
    EXPECT_FALSE(logged.contains("current")) << logged;
    // Text drawn straight into the window after Apply, as Windows' class note was, is not an item:
    // nothing at all is drawn to the right of Apply on its row.
    ASSERT_NE(drawList, nullptr);
    int beyondApply = 0;
    for (const ImDrawVert& vertex : drawList->VtxBuffer)
    {
        if (vertex.pos.x > applyMax.x + 1.0F && vertex.pos.y >= applyMin.y && vertex.pos.y <= applyMax.y)
        {
            ++beyondApply;
        }
    }
    EXPECT_EQ(beyondApply, 0);
}

// The nice slider is the Linux control; Windows draws the priority-class combo instead (#1204), tested
// at the end of this file.
#ifndef _WIN32
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
// The Windows priority-class combo (#1204, #1566): the class picked from its popup is what Apply
// sets, and a Realtime class set outside TaskSmack is shown with a warning, never offered.

/// The centre of row @p index of the open combo popup. The rows are text-high Selectables,
/// ItemSpacing.y apart, from the popup's first content position. Valid from the popup's second
/// frame: on its first it is still being sized and placed.
[[nodiscard]] ImVec2 comboRowCentre(std::size_t index)
{
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    if (g.OpenPopupStack.empty() || g.OpenPopupStack.back().Window == nullptr)
    {
        return {-1.0F, -1.0F};
    }
    const ImGuiWindow& popup = *g.OpenPopupStack.back().Window;
    const float row = ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y;
    return {popup.DC.CursorStartPos.x + 10.0F,
            popup.DC.CursorStartPos.y + (static_cast<float>(index) * row) + (ImGui::GetFontSize() * 0.5F)};
}

TEST_F(ProcessPriorityViewRenderTest, PickingAClassInTheComboAndApplyingSetsThatClass)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    const std::int32_t normal = Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::Normal);
    const ImVec2 apply = renderAndFindApply(view, mock, normal, TARGET_A);

    // The combo follows the "Priority" label on its line, ItemSpacing.x after it.
    ImVec2 combo;
    const auto body = [&]
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        combo = ImVec2(start.x + ImGui::CalcTextSize("Priority").x + ImGui::GetStyle().ItemSpacing.x + 10.0F,
                       start.y + (ImGui::GetFrameHeight() * 0.5F));
        view.render(&mock, CAN_SET_PRIORITY, normal, TARGET_A);
    };
    runFrame(body);
    click(combo, body);
    ASSERT_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty()) << "the click should open the combo";
    runFrame(body); // A new popup is sized and placed on its first frame; its rows settle on the next

    // The rows are SETTABLE_WINDOWS_PRIORITY_CLASSES in order: High is the last.
    const std::size_t highRow = Detail::SETTABLE_WINDOWS_PRIORITY_CLASSES.size() - 1;
    ASSERT_EQ(Detail::SETTABLE_WINDOWS_PRIORITY_CLASSES[highRow], Detail::WindowsPriorityClass::High);
    click(comboRowCentre(highRow), body);
    const std::int32_t high = Detail::windowsPriorityClassNice(Detail::WindowsPriorityClass::High);
    EXPECT_TRUE(ImGui::GetCurrentContext()->OpenPopupStack.empty()) << "a pick closes the combo";
    EXPECT_EQ(view.niceValue(), high);
    EXPECT_TRUE(view.hasPendingEdit());
    EXPECT_EQ(mock.setPriorityCount(), 0); // Picking alone sends nothing

    click(apply, body);
    ASSERT_EQ(mock.setPriorityCount(), 1);
    EXPECT_EQ(mock.lastSetPriorityNice(), high);
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
