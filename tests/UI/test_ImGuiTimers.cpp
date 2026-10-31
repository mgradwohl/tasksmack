/// @file test_ImGuiTimers.cpp
/// @brief Dear ImGui's own timers, for a frame loop that draws static content only when something
/// changes (#3156): when the text cursor next blinks, when a hover delay elapses, and the frames a
/// window or table needs to settle its layout, first as pure decisions and then read from a headless
/// ImGui context.

#include "Core/AnimationRequest.h"
#include "UI/ImGuiTimers.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <cmath>
#include <functional>
#include <limits>

namespace UI
{
namespace
{

constexpr double INF = std::numeric_limits<double>::infinity();

/// The style's delays, as ImGui sets them by default.
ImGuiTimerState hovering()
{
    return ImGuiTimerState{.hoverDelayShortSeconds = 0.15, .hoverDelayNormalSeconds = 0.40, .hoverStationarySeconds = 0.15};
}

// ---- The pure decisions ----

TEST(ImGuiTimersTest, TheCursorBlinksOnTheEdgesOfItsPhase)
{
    // Shown for 0.3 s after a keystroke (the timer starts at -0.3), then 0.8 s on, 0.4 s off.
    EXPECT_NEAR(secondsUntilCursorBlink(-0.3), 1.1, 1e-9);
    EXPECT_NEAR(secondsUntilCursorBlink(0.0), 0.8, 1e-9);
    EXPECT_NEAR(secondsUntilCursorBlink(0.5), 0.3, 1e-9);
    EXPECT_NEAR(secondsUntilCursorBlink(0.9), 0.3, 1e-9); // off: back on at 1.2
    EXPECT_NEAR(secondsUntilCursorBlink(1.3), 0.7, 1e-9); // the second period
    EXPECT_EQ(secondsUntilCursorBlink(std::numeric_limits<double>::quiet_NaN()), INF);
}

TEST(ImGuiTimersTest, NothingRunningAsksForNoFrame)
{
    EXPECT_EQ(secondsUntilImGuiTimerChange({}), INF);
    // A blink timer with no active text input, and hover timers with nothing hovered, count for nothing.
    ImGuiTimerState idle = hovering();
    idle.cursorBlinkSeconds = 0.5;
    idle.hoverDelaySeconds = 0.1;
    EXPECT_EQ(secondsUntilImGuiTimerChange(idle), INF);
}

TEST(ImGuiTimersTest, AnActiveTextInputAsksForItsNextBlink)
{
    ImGuiTimerState state;
    state.cursorBlinking = true;
    state.cursorBlinkSeconds = 0.5;
    EXPECT_NEAR(secondsUntilImGuiTimerChange(state), 0.3 + IMGUI_TIMER_MARGIN_SECONDS, 1e-9);
}

TEST(ImGuiTimersTest, ADelayedTooltipAsksForTheFrameItsDelayEnds)
{
    ImGuiTimerState state = hovering();
    state.hoverDelayPending = true;
    state.hoverDelaySeconds = 0.05;
    state.mouseStationarySeconds = 0.05;
    // The stationary delay and DelayShort both end 0.1 s on.
    EXPECT_NEAR(secondsUntilImGuiTimerChange(state), 0.1 + IMGUI_TIMER_MARGIN_SECONDS, 1e-9);
    // Past both, DelayNormal's 0.4 s is next.
    state.hoverDelaySeconds = 0.2;
    state.mouseStationarySeconds = 0.2;
    EXPECT_NEAR(secondsUntilImGuiTimerChange(state), 0.2 + IMGUI_TIMER_MARGIN_SECONDS, 1e-9);
    // Every delay passed: nothing more to show, so no more frames.
    state.hoverDelaySeconds = 0.5;
    EXPECT_EQ(secondsUntilImGuiTimerChange(state), INF);
}

TEST(ImGuiTimersTest, AHoveredItemAsksForTheMenuHoverOpenOnceTheMouseIsStill)
{
    ImGuiTimerState state;
    state.itemHovered = true;
    state.hoveredIdSeconds = 0.25;
    state.mouseStationarySeconds = 0.1;
    // Both must have passed: the later one decides.
    EXPECT_NEAR(secondsUntilImGuiTimerChange(state), 0.2 + IMGUI_TIMER_MARGIN_SECONDS, 1e-9);
    state.hoveredIdSeconds = 1.0;
    state.mouseStationarySeconds = 1.0;
    EXPECT_EQ(secondsUntilImGuiTimerChange(state), INF);
}

TEST(ImGuiTimersTest, ASettlingLayoutAsksForTheNextFrame)
{
    ImGuiTimerState state;
    state.layoutSettling = true;
    state.cursorBlinking = true;
    state.cursorBlinkSeconds = 0.1;
    EXPECT_EQ(secondsUntilImGuiTimerChange(state), 0.0);
}

// ---- Read from a headless ImGui context ----

class ImGuiTimersContextTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800.0F, 600.0F);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.Fonts->AddFontDefault();
        static_cast<void>(Core::AnimationRequest::consume());
        static_cast<void>(Core::AnimationRequest::consumeFrameWithin());
    }

    void TearDown() override
    {
        static_cast<void>(Core::AnimationRequest::consume());
        static_cast<void>(Core::AnimationRequest::consumeFrameWithin());
        ImGui::DestroyContext(m_Context);
    }

    /// One frame drawing @p body in a fixed window; returns the timers read after it, as UILayer reads
    /// them, and asks for the frames as UILayer does.
    static ImGuiTimerState runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(400.0F, 300.0F));
        ImGui::Begin("Timers", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        const ImGuiTimerState state = currentImGuiTimerState();
        requestFramesForImGuiTimers();
        ImGui::Render();
        return state;
    }

  private:
    ImGuiContext* m_Context = nullptr;
};

TEST_F(ImGuiTimersContextTest, ANewWindowSettlesOverItsFirstFramesThenAsksForNothing)
{
    // A window sized by its content, as a dialog or tooltip is: hidden on its first frame while ImGui
    // measures it, then auto-fitted over the next.
    const auto body = []
    {
        ImGui::Begin("Dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted("static");
        ImGui::End();
    };
    EXPECT_TRUE(runFrame(body).layoutSettling) << "a window is hidden on its first frame";
    EXPECT_EQ(Core::AnimationRequest::consumeFrameWithin(), 0.0);
    bool settled = false;
    for (int frame = 0; frame < 5 && !settled; ++frame)
    {
        settled = !runFrame(body).layoutSettling;
    }
    EXPECT_TRUE(settled);
    EXPECT_EQ(Core::AnimationRequest::consumeFrameWithin(), INF) << "a settled static window asks for no frame";
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0);
}

TEST_F(ImGuiTimersContextTest, ATableAutoFittingItsColumnsAsksForTheNextFrames)
{
    bool settling = false;
    const auto body = [&settling]
    {
        if (ImGui::BeginTable("cells", 2))
        {
            ImGui::TableSetupColumn("a", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("b", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("one");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("two");
            ImGui::EndTable();
        }
        settling = layoutNeedsAnotherFrame(*ImGui::GetCurrentContext());
    };
    static_cast<void>(runFrame(body));
    EXPECT_TRUE(settling) << "unsized columns auto-fit over their first frames";
    bool settled = false;
    for (int frame = 0; frame < 6 && !settled; ++frame)
    {
        settled = !runFrame(body).layoutSettling;
    }
    EXPECT_TRUE(settled);
}

TEST_F(ImGuiTimersContextTest, AnActiveTextInputAsksForItsBlinkAndReadsTheStyle)
{
    char text[16] = "abc"; // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays) - ImGui::InputText's buffer
    const auto body = [&text]
    {
        ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##field", text, sizeof(text));
    };
    ImGuiTimerState state;
    for (int frame = 0; frame < 4; ++frame)
    {
        state = runFrame(body);
    }
    EXPECT_TRUE(state.cursorBlinking);
    EXPECT_DOUBLE_EQ(state.hoverDelayNormalSeconds, static_cast<double>(ImGui::GetStyle().HoverDelayNormal));
    const double within = Core::AnimationRequest::consumeFrameWithin();
    EXPECT_TRUE(std::isfinite(within));
    EXPECT_LE(within, CURSOR_BLINK_PERIOD_SECONDS + IMGUI_TIMER_MARGIN_SECONDS);
}

TEST_F(ImGuiTimersContextTest, AHeldMouseButtonKeepsTheIdleRate)
{
    const auto body = []
    {
        ImGui::TextUnformatted("static");
    };
    for (int frame = 0; frame < 4; ++frame)
    {
        static_cast<void>(runFrame(body));
    }
    static_cast<void>(Core::AnimationRequest::consume());
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(runFrame(body));
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), HELD_MOUSE_BUTTON_FPS);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(runFrame(body));
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0);
}

} // namespace
} // namespace UI
