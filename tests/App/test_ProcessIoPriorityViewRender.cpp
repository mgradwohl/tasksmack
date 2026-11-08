/// @file test_ProcessIoPriorityViewRender.cpp
/// @brief The I/O priority control's real ImGui render, headless (#803), drawn as Process Details draws
/// it: by ProcessPriorityView, under the nice control. It is hidden (and nothing is read) without the
/// capability, as on Windows; it reads the shown process's I/O priority; Apply is disabled until an
/// edit; a click on Apply sets the edit on the selected process once; an edit made for another process
/// is dropped before it can be applied; and a failed apply shows the error line.

#include "App/Panels/ProcessIoPriorityView.h"
#include "App/Panels/ProcessPriorityView.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // SetFocusID(), ActivateItemByID(), ImHashStr(): the slider and Reset by ID

#include <cmath>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>

namespace App
{
namespace
{

using Platform::IoPriority;
using Platform::IoPriorityClass;

constexpr Platform::ProcessTarget TARGET_A{.pid = 1001, .startTimeTicks = 5000};
constexpr Platform::ProcessTarget TARGET_B{.pid = 2002, .startTimeTicks = 6000};

constexpr Platform::ProcessActionCapabilities NICE_ONLY{.canSetPriority = true};
constexpr Platform::ProcessActionCapabilities NICE_AND_IO{.canSetPriority = true, .canSetIoPriority = true};

constexpr IoPriority BEST_EFFORT_4{.ioClass = IoPriorityClass::BestEffort, .level = 4};
constexpr IoPriority IDLE{.ioClass = IoPriorityClass::Idle, .level = 0};
constexpr std::optional<std::int32_t> NICE = 0;

class ProcessIoPriorityViewRenderTest : public ::testing::Test
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
    /// The I/O slider's ID in the window runFrame() draws into: under the view's "io_priority" scope.
    [[nodiscard]] static ImGuiID ioSliderId()
    {
        return ioSliderIdIn(ImGui::GetCurrentWindow());
    }

    /// The I/O slider's ID in @p window, from outside a frame.
    [[nodiscard]] static ImGuiID ioSliderIdIn(const ImGuiWindow* window)
    {
        const ImGuiID scope = ImHashStr("io_priority", 0, window->ID);
        return ImHashStr(Detail::IO_PRIORITY_SLIDER_ID, 0, scope);
    }

    /// Focuses the I/O slider, then presses and releases @p key, drawing @p body each frame.
    static void pressOnIoSlider(ImGuiKey key, const std::function<void()>& body)
    {
        bool focus = true;
        const auto focused = [&]
        {
            if (focus)
            {
                ImGui::SetFocusID(ioSliderId(), ImGui::GetCurrentWindow());
                focus = false;
            }
            body();
        };
        runFrame(focused);
        runFrame(focused);
        ImGui::GetIO().AddKeyEvent(key, true);
        runFrame(focused);
        ImGui::GetIO().AddKeyEvent(key, false);
        runFrame(focused);
    }

    /// One frame of @p view as Process Details runs it (#2734): the update path's I/O priority read
    /// first (waited for, so the tests are deterministic), then render(). A read starts on the frame
    /// after the control is first drawn and is taken in the same frame.
    static void draw(ProcessPriorityView& view,
                     TestMocks::MockProcessActions& mock,
                     const Platform::ProcessActionCapabilities& capabilities,
                     std::optional<std::int32_t> currentNice,
                     const Platform::ProcessTarget& target)
    {
        Platform::IProcessActions* actions = capabilities.canSetIoPriority ? &mock : nullptr;
        if (view.updateReads(actions, target, FRAME_SECONDS, /*wait=*/true))
        {
            (void) view.updateReads(actions, target, 0.0F, /*wait=*/true);
        }
        view.render(&mock, capabilities, currentNice, target);
    }

    static constexpr float FRAME_SECONDS = 1.0F / 60.0F;

    static void runFrame(const std::function<void()>& body, float windowWidth = 1600.0F)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(windowWidth, 1000.0F));
        ImGui::Begin("Details", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// Renders @p view for @p target and returns the centre of the I/O Apply button: the last item
    /// drawn while neither error line shows.
    static ImVec2
    renderAndFindIoApply(ProcessPriorityView& view, TestMocks::MockProcessActions& mock, const Platform::ProcessTarget& target)
    {
        ImVec2 centre;
        runFrame(
            [&]
            {
                draw(view, mock, NICE_AND_IO, NICE, target);
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

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ProcessIoPriorityViewRenderTest, HiddenAndNotReadWithoutTheCapability)
{
    TestMocks::MockProcessActions mock;
    ProcessPriorityView view;
    runFrame([&] { draw(view, mock, NICE_ONLY, NICE, TARGET_A); });
    EXPECT_EQ(mock.getIoPriorityCount(), 0);
    EXPECT_FALSE(view.ioPriorityView().currentIoPriority().has_value());
}

// #2734: render() never reads. The first frame draws the control; the next frame's update path reads the
// process's value on the worker, and that frame shows it.
TEST_F(ProcessIoPriorityViewRenderTest, TheFrameAfterTheControlShowsReadsAndShowsTheProcessValue)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(IDLE);
    ProcessPriorityView view;
    runFrame([&] { view.render(&mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(mock.getIoPriorityCount(), 0); // render() alone reads nothing
    runFrame([&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(mock.getIoPriorityCount(), 1);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), IDLE);
    EXPECT_FALSE(view.ioPriorityView().hasPendingEdit());
}

TEST_F(ProcessIoPriorityViewRenderTest, ApplyIsDisabledUntilAnEdit)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    click(apply, [&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    view.update(/*wait=*/true); // the apply runs on its worker (#2305)
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_EQ(mock.setPriorityCount(), 0);
}

TEST_F(ProcessIoPriorityViewRenderTest, ClickingApplySetsTheEditOnTheSelectedProcessOnce)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    runFrame([&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); }); // Reads the current value
    view.ioPriorityView().editIoPriority(IDLE, TARGET_A);
    // Found with the edit shown: Idle has no level slider, so Apply sits further left than for Best-effort.
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);

    click(apply, [&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    view.update(/*wait=*/true); // the apply runs on its worker (#2305)
    ASSERT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(mock.setPriorityCount(), 0); // The nice control's Apply is a different button
    EXPECT_EQ(mock.lastTarget().pid, TARGET_A.pid);
    EXPECT_EQ(mock.lastTarget().startTimeTicks, TARGET_A.startTimeTicks);
    EXPECT_EQ(mock.lastSetIoPriority(), IDLE);

    click(apply, [&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); }); // Disabled again
    view.update(/*wait=*/true);                                           // the apply runs on its worker (#2305)
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
}

TEST_F(ProcessIoPriorityViewRenderTest, AnEditForAnotherProcessIsDroppedBeforeApply)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    // Found before the edit, with Best-effort's level slider shown: where Apply is once B (also
    // Best-effort) is shown and the edit for A dropped.
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    view.ioPriorityView().editIoPriority(IDLE, TARGET_A);

    click(apply, [&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_B); });
    view.update(/*wait=*/true); // the apply runs on its worker (#2305)
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
    EXPECT_FALSE(view.ioPriorityView().hasPendingEdit());
}

TEST_F(ProcessIoPriorityViewRenderTest, AFailedApplyShowsTheErrorLine)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    mock.setIoPriorityResult(Platform::ProcessActionResult::error("Permission denied"));
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    view.ioPriorityView().editIoPriority({.ioClass = IoPriorityClass::Realtime, .level = 0}, TARGET_A);

    click(apply, [&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    view.update(/*wait=*/true); // the apply runs on its worker (#2305)
    EXPECT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(view.ioPriorityView().error(), "Permission denied");
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), BEST_EFFORT_4);
    EXPECT_TRUE(view.error().empty()); // The nice control's error line is its own
}

// #2308, measured by #2417: a refusal an administrator could redo is drawn with its error line and an
// "Authenticate and retry" button under it (the last item drawn), and pressing it retries as one.
TEST_F(ProcessIoPriorityViewRenderTest, ARefusalShowsAuthenticateAndRetryWhichRetriesAsAnAdministrator)
{
    constexpr Platform::ProcessActionCapabilities AUTHORIZABLE{
        .canSetPriority = true, .canSetIoPriority = true, .canAuthorizeInteractively = true};
    TestMocks::MockProcessActions mock;
    mock.setCapabilities(AUTHORIZABLE);
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    mock.setIoPriorityResult(Platform::ProcessActionResult::denied("Permission denied"));
    ProcessPriorityView view;
    const ImVec2 apply = renderAndFindIoApply(view, mock, TARGET_A);
    view.ioPriorityView().editIoPriority(IDLE, TARGET_A);
    click(apply, [&] { draw(view, mock, AUTHORIZABLE, NICE, TARGET_A); });
    view.update(/*wait=*/true);
    ASSERT_EQ(view.ioPriorityView().error(), "Permission denied");
    ASSERT_TRUE(view.ioPriorityView().offersAuthenticateAndRetry());

    const ImVec2 retry = renderAndFindIoApply(view, mock, TARGET_A); // now the retry button
    click(retry, [&] { draw(view, mock, AUTHORIZABLE, NICE, TARGET_A); });
    view.update(/*wait=*/true);
    EXPECT_EQ(mock.runAuthorizedCount(), 1);
    EXPECT_EQ(mock.lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::SetIoPriority);
    EXPECT_EQ(mock.lastAuthorizedAction().ioClass, IoPriorityClass::Idle);
    EXPECT_TRUE(view.ioPriorityView().error().empty());
}

// The error line of a read that failed (not an apply) is drawn muted, under the control.
TEST_F(ProcessIoPriorityViewRenderTest, AFailedReadShowsItsMessage)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(std::unexpected(std::string("Couldn't read the I/O priority")));
    ProcessPriorityView view;
    runFrame([&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    runFrame([&] { draw(view, mock, NICE_AND_IO, NICE, TARGET_A); });
    EXPECT_EQ(view.ioPriorityView().readError(), "Couldn't read the I/O priority");
    EXPECT_FALSE(view.ioPriorityView().currentIoPriority().has_value());
    EXPECT_GE(mock.getIoPriorityCount(), 1);
}

// As wide as its label, as the nice control's Apply and Terminate and Kill are (#1537): its old em
// floor made it a wide muted bar while disabled.
TEST_F(ProcessIoPriorityViewRenderTest, ApplyIsAsWideAsItsLabel)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    float applyWidth = 0.0F;
    float labelWidth = 0.0F;
    runFrame(
        [&]
        {
            draw(view, mock, NICE_AND_IO, NICE, TARGET_A);
            applyWidth = ImGui::GetItemRectSize().x; // The I/O Apply is the last item while no error line shows
            labelWidth = ImGui::CalcTextSize(ICON_FA_CHECK "  Apply").x + (2.0F * ImGui::GetStyle().FramePadding.x);
        });
    EXPECT_GT(applyWidth, 0.0F);
    EXPECT_LE(applyWidth, std::ceil(labelWidth));
    // The nice control's label, check mark included (#2005): as wide as that, not as a bare "Apply".
    EXPECT_GE(applyWidth, std::floor(labelWidth));
}

TEST_F(ProcessIoPriorityViewRenderTest, ApplyStaysInsideANarrowPanel)
{
    // Process Details does not scroll horizontally: at a narrow width the row shrinks, then wraps,
    // so Apply is never clipped off the right edge.
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4); // Explicit Best-effort: the slider, then Reset to default and Apply
    ProcessPriorityView view;
    for (const float width : {420.0F, 300.0F, 160.0F})
    {
        SCOPED_TRACE(width);
        float applyRight = 0.0F;
        float windowRight = 0.0F;
        const auto body = [&]
        {
            draw(view, mock, NICE_AND_IO, NICE, TARGET_A);
            applyRight = ImGui::GetItemRectMax().x;
            windowRight = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;
        };
        runFrame(body, width);
        runFrame(body, width);
        EXPECT_LE(applyRight, windowRight);
    }
}

// One slider for class and level (#1540): no class combo, and its keys step across the class bands --
// Best-effort 7 to Idle, and (with CAP_SYS_NICE) Best-effort 0 to Realtime 7.
TEST_F(ProcessIoPriorityViewRenderTest, TheSliderStepsAcrossTheClassBands)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(Platform::IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 7});
    ProcessPriorityView view;
    const auto body = [&]
    {
        draw(view, mock, NICE_AND_IO, NICE, TARGET_A);
    };
    runFrame(body); // Reads the current value
    pressOnIoSlider(ImGuiKey_RightArrow, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), IDLE);
    EXPECT_TRUE(view.ioPriorityView().hasPendingEdit());
    pressOnIoSlider(ImGuiKey_LeftArrow, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority().ioClass, IoPriorityClass::BestEffort);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority().level, 7);
    EXPECT_EQ(mock.setIoPriorityCount(), 0); // Picking applies nothing
}

TEST_F(ProcessIoPriorityViewRenderTest, RealtimeIsOnTheSliderOnlyWithThePrivilege)
{
    for (const bool privileged : {false, true})
    {
        SCOPED_TRACE(privileged ? "CAP_SYS_NICE" : "unprivileged");
        TestMocks::MockProcessActions mock;
        mock.setIoPriorityReadResult(Platform::IoPriority{.ioClass = IoPriorityClass::BestEffort, .level = 0});
        Platform::ProcessActionCapabilities capabilities = NICE_AND_IO;
        capabilities.canSetRealtimeIoPriority = privileged;
        ProcessPriorityView view;
        const auto body = [&]
        {
            draw(view, mock, capabilities, NICE, TARGET_A);
        };
        runFrame(body);
        pressOnIoSlider(ImGuiKey_LeftArrow, body); // Toward the high end, from Best-effort 0
        const Platform::IoPriority shown = view.ioPriorityView().shownIoPriority();
        if (privileged)
        {
            EXPECT_EQ(shown.ioClass, IoPriorityClass::Realtime);
            EXPECT_EQ(shown.level, 7);
        }
        else
        {
            EXPECT_EQ(shown.ioClass, IoPriorityClass::BestEffort); // Held at the start: no Realtime band
            EXPECT_EQ(shown.level, 0);
            EXPECT_FALSE(view.ioPriorityView().hasPendingEdit());
        }
    }
}

TEST_F(ProcessIoPriorityViewRenderTest, ResetToDefaultFollowsTheNiceValueAgain)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(BEST_EFFORT_4);
    ProcessPriorityView view;
    const auto body = [&]
    {
        draw(view, mock, NICE_AND_IO, NICE, TARGET_A);
    };
    runFrame(body);
    runFrame(body);
    const ImGuiID resetId = ImHashStr("Reset to default", 0, ImHashStr("io_priority", 0, ImGui::FindWindowByName("Details")->ID));
    ImGui::ActivateItemByID(resetId);
    runFrame(body);
    runFrame(body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority().ioClass, IoPriorityClass::None);
    EXPECT_TRUE(view.ioPriorityView().hasPendingEdit());
    EXPECT_EQ(mock.setIoPriorityCount(), 0);
}

// --- Windows' hints (#3362) ---------------------------------------------------------------------------------

constexpr Platform::ProcessActionCapabilities NICE_AND_HINTS{.canSetPriority = true, .canSetIoPriority = true, .ioPriorityHints = true};

[[nodiscard]] constexpr IoPriority hint(Platform::IoPriorityHint value)
{
    return {.ioClass = IoPriorityClass::WindowsHint, .level = static_cast<std::int32_t>(value)};
}

TEST_F(ProcessIoPriorityViewRenderTest, WindowsHintsStepFromHighToVeryLowAndApplyTheHint)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(hint(Platform::IoPriorityHint::Normal));
    ProcessPriorityView view;
    const auto body = [&]
    {
        draw(view, mock, NICE_AND_HINTS, NICE, TARGET_A);
    };
    runFrame(body); // Draws the control
    runFrame(body); // Reads the current value
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::Normal));
    pressOnIoSlider(ImGuiKey_RightArrow, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::Low));
    pressOnIoSlider(ImGuiKey_End, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::VeryLow));
    pressOnIoSlider(ImGuiKey_Home, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::High));
    EXPECT_EQ(mock.setIoPriorityCount(), 0); // Picking applies nothing

    view.ioPriorityView().apply(&mock, TARGET_A);
    view.update(/*wait=*/true);
    ASSERT_EQ(mock.setIoPriorityCount(), 1);
    EXPECT_EQ(mock.lastSetIoPriority(), hint(Platform::IoPriorityHint::High));
}

TEST_F(ProcessIoPriorityViewRenderTest, AReadRefusedForAccessEnablesApplyOnlyWhereTheHelperCanRetry)
{
    for (const bool helper : {false, true})
    {
        SCOPED_TRACE(helper ? "helper" : "no helper");
        TestMocks::MockProcessActions mock;
        mock.setCapabilities(
            {.canSetPriority = true, .canSetIoPriority = true, .ioPriorityHints = true, .canAuthorizeInteractively = helper});
        mock.setIoPriorityReadResult(std::unexpected(Platform::ioPriorityReadDeniedMessage(TARGET_A.pid)));
        ProcessPriorityView view;
        const auto body = [&]
        {
            draw(view, mock, NICE_AND_HINTS, NICE, TARGET_A);
        };
        runFrame(body);
        runFrame(body);
        view.ioPriorityView().editIoPriority(hint(Platform::IoPriorityHint::VeryLow), TARGET_A);
        runFrame(body);
        EXPECT_EQ(view.ioPriorityView().canApply(TARGET_A), helper);
    }
}

TEST_F(ProcessIoPriorityViewRenderTest, ClickingNormalAfterARefusedReadQueuesNormal)
{
    // The read refused, the slider shows Normal as a stand-in for the unknown value; clicking Normal itself must
    // still be an edit to the Normal hint, so Apply can send it through the helper.
    TestMocks::MockProcessActions mock;
    mock.setCapabilities({.canSetPriority = true, .canSetIoPriority = true, .ioPriorityHints = true, .canAuthorizeInteractively = true});
    mock.setIoPriorityReadResult(std::unexpected(Platform::ioPriorityReadDeniedMessage(TARGET_A.pid)));
    ProcessPriorityView view;
    const auto body = [&]
    {
        draw(view, mock, NICE_AND_HINTS, NICE, TARGET_A);
    };
    runFrame(body);
    runFrame(body);
    ASSERT_FALSE(view.ioPriorityView().currentIoPriority().has_value());
    ASSERT_EQ(Detail::ioHintStopFor(view.ioPriorityView().shownIoPriority()), 1) << "Normal shown as the stand-in";

    // Find the I/O slider's track by hovering: a row of it, then its left and right ends.
    const auto hoveredAt = [&](float x, float y)
    {
        ImGui::GetIO().AddMousePosEvent(x, y);
        runFrame(body);
        return ImGui::GetHoveredID() == ioSliderIdIn(ImGui::FindWindowByName("Details"));
    };
    constexpr float PROBE_X = 200.0F;
    float row = -1.0F;
    for (int step = 0; step < 300 && row < 0.0F; ++step)
    {
        const float y = 2.0F * static_cast<float>(step);
        if (hoveredAt(PROBE_X, y))
        {
            row = y;
        }
    }
    ASSERT_GE(row, 0.0F) << "the I/O slider was never hovered";
    float left = PROBE_X;
    while (left > 0.0F && hoveredAt(left - 1.0F, row))
    {
        left -= 1.0F;
    }
    float right = PROBE_X;
    while (right < 1600.0F && hoveredAt(right + 1.0F, row))
    {
        right += 1.0F;
    }
    ASSERT_GT(right - left, 30.0F);

    // Normal is the second of four stops: a third of the way along the track.
    click(ImVec2(left + ((right - left) / 3.0F), row), body);
    EXPECT_TRUE(view.ioPriorityView().hasPendingEdit());
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::Normal));
    EXPECT_TRUE(view.ioPriorityView().canApply(TARGET_A));
}

TEST_F(ProcessIoPriorityViewRenderTest, WindowsHintsOfferNoResetAndNeverCritical)
{
    TestMocks::MockProcessActions mock;
    mock.setIoPriorityReadResult(hint(Platform::IoPriorityHint::Critical)); // the system's own
    ProcessPriorityView view;
    const auto body = [&]
    {
        draw(view, mock, NICE_AND_HINTS, NICE, TARGET_A);
    };
    runFrame(body);
    runFrame(body);
    const ImGuiID resetId = ImHashStr("Reset to default", 0, ImHashStr("io_priority", 0, ImGui::FindWindowByName("Details")->ID));
    ImGui::ActivateItemByID(resetId);
    runFrame(body);
    runFrame(body);
    EXPECT_FALSE(view.ioPriorityView().hasPendingEdit()) << "no Reset to default on Windows";

    // Critical is shown beyond the start: the slider steps down from it to High, never back to it.
    pressOnIoSlider(ImGuiKey_RightArrow, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::High));
    pressOnIoSlider(ImGuiKey_LeftArrow, body);
    EXPECT_EQ(view.ioPriorityView().shownIoPriority(), hint(Platform::IoPriorityHint::High));
}

} // namespace
} // namespace App
