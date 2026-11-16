/// @file test_UiLayersOnDisplayDrag.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): the title-bar drag the bar runs client-side, and its
/// geometry. Split from test_UiLayersOnDisplay.cpp by feature (#3850); the shared checks and fixture are in UiLayersOnDisplayFixture.h.

#include "App/AboutDialog.h"
#include "App/AboutLayer.h"
#include "App/ElevationNoticeLayer.h"
#include "App/HelpLayer.h"
#include "App/Panels/ProcessDetailsPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SettingsDialog.h"
#include "App/SettingsLayer.h"
#include "App/ShellLayer.h"
#include "App/TitleBarButtons.h"
#include "App/TitleBarGeometry.h"
#include "App/TitleBarLayer.h"
#include "App/UserConfig.h"
#include "App/WindowInteraction.h"
#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/ConfigDirOverride.h"
#include "Core/Event.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/VideoBackend.h"
#include "Core/Window.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "Domain/SamplingConfig.h"
#include "ScratchConfigDir.h"
#include "UI/DpiScale.h"
#include "UI/FontPointSize.h"
#include "UI/IconTextureCache.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UI/UILayer.h"
#include "UiLayersOnDisplayFixture.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>
#include <glad/gl.h>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // FindWindowByName(), ImHashStr(), ActivateItemByID(): the bar and its buttons
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h> // DwmSetWindowAttribute(DWMWA_CLOAK) (#3152)
// clang-format on

#include <cstring> // sendDisplayState()
#include <span>    // sendDisplayState()
#else
#include <unistd.h> // getpid(): the hidden-shell test selects this process (#3156)
#endif

// NOLINTNEXTLINE(google-build-using-namespace) - the shared helpers, as the one file had them
using namespace UiLayersOnDisplayTesting;

namespace
{

struct WindowDragObserved
{
    /// False under SDL's offscreen driver, which keeps a window where it was made, and on a desktop
    /// whose window manager ignored a direct setPosition() after the drag didn't move the window (#2707).
    bool positionsApply = true;
    bool movedByDrag = false;
    bool widenedByResize = false;
    bool maximizedBeforeDrag = false;
    bool restoredByDrag = false;
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    /// The simulated focus loss reached TitleBarLayer after the first press (loseFocusAfterFirstPress).
    bool focusLossDelivered = false;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: holds the left button down and moves the pointer through
/// TitleBarLayer::setMouseStateQueryForTesting() -- a pushed event can't move SDL's own pointer -- so
/// the drag and resize the press begins step each frame (#1891): a drag on the title bar moves the
/// window, a drag from the right edge widens it, and a drag on a maximized window's title bar past
/// the threshold restores it. Each step waits for its effect (the window manager applies a move,
/// resize or restore some frames later on X11) up to WAIT_LIMIT frames, then lets go.
///
/// A press with no effect after RETRY_EVERY frames is made again (#2221). TitleBarLayer ends a drag or
/// resize on SDL_EVENT_WINDOW_FOCUS_LOST, as it should. Under `xvfb-run ctest -j`, every display test
/// shares one X server, and with no window manager focus follows the pointer. So another test's window
/// mapping over this one can take the focus between the press and the first step, and the step never
/// starts. The double-click that maximizes isn't repeated: it toggles, and it doesn't need the focus
/// after the event.
class WindowDragDriverLayer : public Core::Layer
{
  public:
    /// @p loseFocusAfterFirstPress: send the window a focus loss right after the first drag's press,
    /// as another window taking the focus would (#2221). The pointer stays where it pressed until the
    /// focus loss has been handled, and only then moves to where the drag goes (#2901): the frame's
    /// event drain stops at FrameLoopState::DRAIN_BUDGET_MS, so under load the press and the focus loss
    /// can land in different frames, and a drag step between them, with the pointer already moved,
    /// would move the window before the drag was lost and no press would ever be made again.
    WindowDragDriverLayer(WindowDragObserved& observed, App::TitleBarLayer& titleBar, bool loseFocusAfterFirstPress = false)
        : Layer("WindowDragDriver"), m_Observed(observed), m_TitleBar(titleBar), m_LoseFocusAfterFirstPress(loseFocusAfterFirstPress)
    {}

    /// Runs after TitleBarLayer's, for each event: the focus loss pushed after the first press has
    /// ended the drag by the time it gets here.
    void onSDLEvent(SDL_Event* event) override
    {
        if (event == nullptr || m_FocusLoss == FocusLoss::None || m_FocusLoss == FocusLoss::Delivered)
        {
            return;
        }
        const SDL_WindowID windowId = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        if (m_FocusLoss == FocusLoss::Pushed && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.windowID == windowId)
        {
            m_FocusLoss = FocusLoss::PressHandled;
        }
        else if (m_FocusLoss == FocusLoss::PressHandled && event->type == SDL_EVENT_WINDOW_FOCUS_LOST && event->window.windowID == windowId)
        {
            m_FocusLoss = FocusLoss::Delivered;
            m_Observed.focusLossDelivered = true;
        }
    }

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < m_NextFrame)
        {
            return;
        }
        const Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const auto [x, y] = window.getPosition();
        const float titleX = static_cast<float>(width) * 0.5F; // empty title bar: the middle, clear of the buttons
        constexpr float TITLE_Y = 16.0F;
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const auto waiting = [this, frame]
        {
            return frame < m_Deadline;
        };
        // Press again from where the window is now (see the class comment).
        const auto retryDue = [this, frame]
        {
            if (frame - m_PressedAt < RETRY_EVERY)
            {
                return false;
            }
            m_PressedAt = frame;
            ++m_Observed.pressRetries;
            return true;
        };
        switch (m_Phase)
        {
        case Phase::BeginDrag:
        {
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            m_Position = {x, y};
            if (m_LoseFocusAfterFirstPress)
            {
                // Still, until the focus loss has ended the drag (see the constructor).
                hold(x + static_cast<int>(titleX), y + static_cast<int>(TITLE_Y));
                pushButton(titleX, TITLE_Y, true, 1);
                pushFocusLost();
                m_FocusLoss = FocusLoss::Pushed;
            }
            else
            {
                hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                pushButton(titleX, TITLE_Y, true, 1);
            }
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMoved, 1);
            break;
        }
        case Phase::AwaitMoved:
            m_Observed.movedByDrag = std::pair{x, y} != m_Position;
            if ((m_FocusLoss == FocusLoss::Pushed || m_FocusLoss == FocusLoss::PressHandled) && waiting())
            {
                next(Phase::AwaitMoved, 1); // the press and the focus loss aren't both handled yet
                break;
            }
            if (m_FocusLoss == FocusLoss::Delivered && !m_DragTargetHeld)
            {
                // The drag is lost: the pointer moves on with the button held, and only a press made
                // again can move the window, a full RETRY_EVERY frames from now.
                hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                m_DragTargetHeld = true;
                m_PressedAt = frame;
            }
            if (!m_Observed.movedByDrag && m_Observed.positionsApply && waiting())
            {
                if (retryDue())
                {
                    hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                    pushButton(titleX, TITLE_Y, true, 1);
                }
                next(Phase::AwaitMoved, 1);
                break;
            }
            release();
            if (!m_Observed.movedByDrag && m_Observed.positionsApply && !TestSupport::displayRequired())
            {
                // The drag didn't move the window. WSLg's window manager (Weston's XWM) sometimes
                // ignores client moves altogether (#2707), as SetAndGetPositionRoundTrip skips for: ask
                // for a move directly, and only if that lands is the drag at fault. CI
                // (TASKSMACK_REQUIRE_DISPLAY=1, no window manager) always asserts the drag.
                m_Position = {x, y};
                Core::Application::get().getWindow().setPosition(x + DRAG_X, y + DRAG_Y);
                m_Deadline = frame + WAIT_LIMIT;
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            next(Phase::BeginResize, STEP);
            break;
        case Phase::AwaitProbeMove:
            if (std::pair{x, y} == m_Position && waiting())
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = std::pair{x, y} != m_Position;
            next(Phase::BeginResize, STEP);
            break;
        case Phase::BeginResize:
        {
            m_Size = {width, height};
            const float edgeX = static_cast<float>(width) - 2.0F;
            const float edgeY = static_cast<float>(height) * 0.5F;
            hold(x + static_cast<int>(edgeX) + WIDEN_BY, y + static_cast<int>(edgeY));
            pushButton(edgeX, edgeY, true, 1);
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitWidened, 1);
            break;
        }
        case Phase::AwaitWidened:
            m_Observed.widenedByResize = width > m_Size.first;
            if (!m_Observed.widenedByResize && waiting())
            {
                if (retryDue())
                {
                    const float edgeX = static_cast<float>(width) - 2.0F;
                    const float edgeY = static_cast<float>(height) * 0.5F;
                    hold(x + static_cast<int>(edgeX) + WIDEN_BY, y + static_cast<int>(edgeY));
                    pushButton(edgeX, edgeY, true, 1);
                }
                next(Phase::AwaitWidened, 1);
                break;
            }
            release(); // ends the resize, committing the size it last held back
            next(Phase::Maximize, STEP);
            break;
        case Phase::Maximize:
            pushButton(titleX, TITLE_Y, true, 2); // double-click: maximize
            pushButton(titleX, TITLE_Y, false, 2);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            m_Observed.maximizedBeforeDrag = window.isMaximized();
            if (!m_Observed.maximizedBeforeDrag && waiting())
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
            pushButton(titleX, TITLE_Y, true, 1);
            m_PressedAt = frame;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitRestored, 1);
            break;
        case Phase::AwaitRestored:
            m_Observed.restoredByDrag = !window.isMaximized();
            if (!m_Observed.restoredByDrag && waiting())
            {
                if (retryDue())
                {
                    hold(x + static_cast<int>(titleX) + DRAG_X, y + static_cast<int>(TITLE_Y) + DRAG_Y);
                    pushButton(titleX, TITLE_Y, true, 1);
                }
                next(Phase::AwaitRestored, 1);
                break;
            }
            release();
            next(Phase::Stop, STEP);
            break;
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// The pointer at (@p globalX, @p globalY) with the left button down, until release().
    void hold(int globalX, int globalY)
    {
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
    }

    /// The button up where it was: the next update sees no button held and ends the interaction.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    static void pushButton(float x, float y, bool down, int clicks)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = static_cast<Uint8>(clicks);
        event.button.x = x;
        event.button.y = y;
        static_cast<void>(SDL_PushEvent(&event));
    }

    static void pushFocusLost()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        event.window.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        static_cast<void>(SDL_PushEvent(&event));
    }

    /// How far the focus loss pushed after the first press has got (loseFocusAfterFirstPress).
    enum class FocusLoss : std::uint8_t
    {
        None,         ///< Not pushed: loseFocusAfterFirstPress is false, or the first press isn't made yet
        Pushed,       ///< The press and then the focus loss are queued
        PressHandled, ///< TitleBarLayer began the drag
        Delivered,    ///< TitleBarLayer ended it on the focus loss
    };

    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        BeginDrag,
        AwaitMoved,
        AwaitProbeMove,
        BeginResize,
        AwaitWidened,
        Maximize,
        AwaitMaximized,
        AwaitRestored,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;  // frames to wait for an effect before going on
    static constexpr int RETRY_EVERY = 30;  // frames before a press with no effect is made again
    static constexpr int MAX_FRAMES = 1800; // room for every wait, the direct-move probe's included, to run out
    static constexpr int DRAG_X = 40;       // past DragTracker::DRAG_THRESHOLD_PX
    static constexpr int DRAG_Y = 30;
    static constexpr int WIDEN_BY = 60;

    WindowDragObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    bool m_LoseFocusAfterFirstPress = false;
    FocusLoss m_FocusLoss = FocusLoss::None;
    bool m_DragTargetHeld = false; ///< The pointer moved on after the focus loss
    Phase m_Phase = Phase::BeginDrag;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    int m_PressedAt = 0;
    std::pair<int, int> m_Position;
    std::pair<int, int> m_Size;
};

TEST_F(UiLayersOnDisplayTest, TitleBarDragsAndResizesStepWhileTheButtonIsHeld)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if (onNativeWayland())
    {
        GTEST_SKIP() << COMPOSITOR_DRAGS;
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragTest";
    spec.VSync = false;
    WindowDragObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<WindowDragDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    EXPECT_FALSE(observed.timedOut);
    if (observed.positionsApply)
    {
        EXPECT_TRUE(observed.movedByDrag); // a title-bar drag moves the window
    }
    EXPECT_TRUE(observed.widenedByResize);     // a right-edge drag widens it
    EXPECT_TRUE(observed.maximizedBeforeDrag); // (the double-click maximized it)
    EXPECT_TRUE(observed.restoredByDrag);      // and a drag past the threshold restores it
}

// #2221: TitleBarLayer ends a drag on a focus loss, so a focus loss right after the press (another
// test's window mapping over this one, on the X server they share under `xvfb-run ctest -j`) left the
// drag step waiting out its deadline with the window unmoved. The driver presses again. The pointer
// moves only once the focus loss has ended the drag, so the move can only come from that press made
// again, however the frames split the events (#2901).
TEST_F(UiLayersOnDisplayTest, TitleBarDragStepSurvivesAFocusLossRightAfterThePress)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if (onNativeWayland())
    {
        GTEST_SKIP() << COMPOSITOR_DRAGS;
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragFocusTest";
    spec.VSync = false;
    WindowDragObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<WindowDragDriverLayer>(observed, titleBar, /*loseFocusAfterFirstPress=*/true);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.focusLossDelivered) << "TitleBarLayer handled the press, then the focus loss";
    if (observed.positionsApply)
    {
        EXPECT_TRUE(observed.movedByDrag);
        EXPECT_GE(observed.pressRetries, 1) << "the lost drag was pressed again";
    }
}

/// What the drag-geometry driver saw (#2715).
struct DragGeometryObserved
{
    bool positionsApply = true; ///< False under SDL's offscreen driver, which keeps a window where it was made.
    int checks = 0;             ///< Pointer positions checked against where the window ended up
    std::vector<std::string> failures;
    bool widenedByResize = false;
    std::array<bool, 2> maximizedBeforeDrag{}; ///< For the press near the left end, then the right
    std::array<bool, 2> clickKeptMaximized{};  ///< A move inside the threshold leaves it maximized
    std::array<bool, 2> restoredByDrag{};
    bool maximizeUnavailable = false; ///< The window never maximized: the restore checks were skipped
    /// What the window's hit test answers where each press was made (#2716): the path a real press
    /// there takes. The driver's pushed events drive the client-side path whatever the answer.
    SDL_HitTestResult plainPressHitTest = SDL_HITTEST_NORMAL;
    SDL_HitTestResult resizePressHitTest = SDL_HITTEST_NORMAL;
    std::array<SDL_HitTestResult, 2> maximizedPressHitTest{};
    bool systemDrag = false;         ///< Core::VideoBackend::usesSystemDrag() while the app ran
    bool systemResize = false;       ///< Core::VideoBackend::usesSystemResize()
    bool clientSideMaximize = false; ///< Core::VideoBackend::supportsClientSideMaximize()
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: drives the title bar through its own test seams --
/// SDL_PushEvent() for the press and TitleBarLayer::setMouseStateQueryForTesting() for the held
/// pointer -- and checks where the window lands, never moving the OS pointer. In order:
/// - a plain drag: the window follows the pointer 1:1;
/// - a resize from the right edge: the window widens and its left and top edges stay put;
/// - twice, pressing near the title bar's left end and then near its right end: maximize, press,
///   move inside DRAG_THRESHOLD_PX (still maximized), then past it and on: the window is restored
///   with the pointer at the same fraction across it and the same distance below its top (#2715).
/// Each position waits for the window to reach it, up to WAIT_LIMIT frames (a window manager applies
/// moves some frames later on X11), then the miss is recorded.
class DragGeometryDriverLayer : public Core::Layer
{
  public:
    DragGeometryDriverLayer(DragGeometryObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("DragGeometryDriver"), m_Observed(observed), m_TitleBar(titleBar)
    {}

    void onRender() override
    {
        Core::AnimationRequest::request(Core::AnimationRequest::FULL_RATE); // keep frames coming
        const int frame = ++m_Observed.frames;
        if (frame > MAX_FRAMES)
        {
            m_Observed.timedOut = true;
            Core::Application::get().stop();
            return;
        }
        if (frame < m_NextFrame)
        {
            return;
        }
        const Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const auto [x, y] = window.getPosition();
        const bool maximized = window.isMaximized();
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::BeginPlainDrag:
        {
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            m_Observed.systemDrag = Core::VideoBackend::usesSystemDrag();
            m_Observed.systemResize = Core::VideoBackend::usesSystemResize();
            m_Observed.clientSideMaximize = Core::VideoBackend::supportsClientSideMaximize();
            m_Start = {x, y};
            const int pressX = width / 2; // the press's own integer point, so the hit test is asked where it lands
            m_Observed.plainPressHitTest = m_TitleBar.hitTest(static_cast<float>(pressX), static_cast<float>(TITLE_Y));
            press(x, y, pressX, TITLE_Y, frame);
            m_Sample = 0;
            holdAt(PLAIN_OFFSETS[0], frame);
            next(Phase::AwaitPlainDrag, 1);
            break;
        }
        case Phase::AwaitPlainDrag:
        {
            // Still where the last position put it: the drag was lost (see retryDue()), so the press
            // is made again where the pointer was.
            const std::pair<int, int> previous = m_Sample == 0 ? std::pair{0, 0} : PLAIN_OFFSETS.at(m_Sample - 1);
            if (within(x - (m_Start.first + previous.first), y - (m_Start.second + previous.second)) && m_Observed.positionsApply &&
                waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, previous, PLAIN_OFFSETS.at(m_Sample), frame);
            }
            const auto [dx, dy] = PLAIN_OFFSETS.at(m_Sample);
            const int errorX = x - (m_Start.first + dx);
            const int errorY = y - (m_Start.second + dy);
            if (!within(errorX, errorY) && m_Observed.positionsApply && waiting)
            {
                next(Phase::AwaitPlainDrag, 1);
                break;
            }
            check(within(errorX, errorY), std::format("plain drag by ({}, {}): window off by ({}, {})", dx, dy, errorX, errorY));
            if (++m_Sample < PLAIN_OFFSETS.size())
            {
                holdAt(PLAIN_OFFSETS.at(m_Sample), frame);
                next(Phase::AwaitPlainDrag, 1);
                break;
            }
            release();
            next(Phase::BeginResize, STEP);
            break;
        }
        case Phase::BeginResize:
            m_Start = {x, y};
            m_StartWidth = width;
            {
                const int pressY = height / 2;
                m_Observed.resizePressHitTest = m_TitleBar.hitTest(static_cast<float>(width - 2), static_cast<float>(pressY));
                press(x, y, width - 2, pressY, frame);
            }
            holdAt({WIDEN_BY, 0}, frame);
            next(Phase::AwaitResize, 1);
            break;
        case Phase::AwaitResize:
        {
            // Not widened at all: the press was lost (see retryDue()), so it is made again.
            if (width == m_StartWidth && waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, {0, 0}, {WIDEN_BY, 0}, frame);
            }
            const bool widened = std::abs(width - (m_StartWidth + WIDEN_BY)) <= TOLERANCE_PX;
            if (!widened && waiting)
            {
                next(Phase::AwaitResize, 1);
                break;
            }
            m_Observed.widenedByResize = width > m_StartWidth;
            const int errorX = x - m_Start.first;
            const int errorY = y - m_Start.second;
            check(within(errorX, errorY), std::format("right-edge resize moved the left/top edges by ({}, {})", errorX, errorY));
            release();
            m_End = 0;
            next(Phase::Maximize, STEP);
            break;
        }
        case Phase::Maximize:
            // A double-click toggles, so it is only sent to a window that isn't maximized already.
            if (!maximized)
            {
                pushButton(static_cast<float>(width) * 0.5F, static_cast<float>(TITLE_Y), true, 2); // double-click
                pushButton(static_cast<float>(width) * 0.5F, static_cast<float>(TITLE_Y), false, 2);
            }
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            m_Observed.maximizedBeforeDrag.at(m_End) = maximized;
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            if (!maximized)
            {
                // This display can't maximize the window (the test skips this part, not the rest).
                m_Observed.maximizeUnavailable = true;
                next(Phase::Stop, STEP);
                break;
            }
            m_Geometry = {};
            m_StableFrames = 0;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximizedSettled, 1);
            break;
        case Phase::AwaitMaximizedSettled:
        {
            // The maximized flag can lead the window's new position and size by some frames (X11
            // reports the configure later), and a press read from the old geometry lands somewhere
            // else on the window: wait until the geometry has held for SETTLED_FRAMES.
            const std::array<int, 4> geometry{x, y, width, height};
            m_StableFrames = geometry == m_Geometry ? m_StableFrames + 1 : 0;
            m_Geometry = geometry;
            if (m_StableFrames < SETTLED_FRAMES && waiting)
            {
                next(Phase::AwaitMaximizedSettled, 1);
                break;
            }
            // Near one end of the title bar, clear of the icon and the buttons: near the top of the
            // title bar for the left end, three quarters of the way down it for the right (a
            // maximized window has no resize border to avoid).
            m_PressLocalY = m_End == 0 ? TITLE_Y_NEAR_TOP : static_cast<int>(App::TitleBarLayer::height() * 0.75F);
            m_PressLocalX = clearPressX(width, m_PressLocalY, m_End == 0);
            m_Fraction = static_cast<double>(m_PressLocalX) / static_cast<double>(width);
            m_Observed.maximizedPressHitTest.at(m_End) =
                m_TitleBar.hitTest(static_cast<float>(m_PressLocalX), static_cast<float>(m_PressLocalY));
            press(x, y, m_PressLocalX, m_PressLocalY, frame);
            holdAt({INSIDE_THRESHOLD, INSIDE_THRESHOLD}, frame);
            next(Phase::CheckStillMaximized, STEP);
            break;
        }
        case Phase::CheckStillMaximized:
            m_Observed.clickKeptMaximized.at(m_End) = maximized;
            m_Sample = 0;
            holdAt(RESTORE_OFFSETS[0], frame);
            next(Phase::AwaitRestoreSample, 1);
            break;
        case Phase::AwaitRestoreSample:
        {
            // Where the pointer is on the window, against where it was on the maximized one.
            const auto pointerError = [&](std::pair<int, int> offset)
            {
                const int pointerX = m_Press.first + offset.first;
                const int pointerY = m_Press.second + offset.second;
                return std::pair{static_cast<int>(std::lround(static_cast<double>(pointerX - x) - (m_Fraction * width))),
                                 (pointerY - y) - m_PressLocalY};
            };
            // Still maximized well after the pointer passed the threshold, or still where the last
            // position put it: the press or the drag was lost (see retryDue()), so the press is made
            // again where the pointer was.
            const std::pair<int, int> previous = m_Sample == 0 ? std::pair{0, 0} : RESTORE_OFFSETS.at(m_Sample - 1);
            const bool lost = m_Sample == 0 ? maximized : !maximized && within(pointerError(previous).first, pointerError(previous).second);
            if (lost && waiting && retryDue(frame))
            {
                retryPress(x, y, width, maximized, previous, RESTORE_OFFSETS.at(m_Sample), frame);
            }
            const auto [dx, dy] = RESTORE_OFFSETS.at(m_Sample);
            const auto [errorX, errorY] = pointerError({dx, dy});
            const bool placed = !maximized && within(errorX, errorY);
            if (!placed && (m_Observed.positionsApply || maximized) && waiting)
            {
                next(Phase::AwaitRestoreSample, 1);
                break;
            }
            if (m_Sample == 0)
            {
                m_Observed.restoredByDrag.at(m_End) = !maximized;
            }
            check(within(errorX, errorY),
                  std::format("{} end, pointer moved by ({}, {}): {:.3f} across a {}-wide window, {} px down; pointer off by ({}, {})",
                              m_End == 0 ? "left" : "right",
                              dx,
                              dy,
                              m_Fraction,
                              width,
                              m_PressLocalY,
                              errorX,
                              errorY));
            if (++m_Sample < RESTORE_OFFSETS.size())
            {
                holdAt(RESTORE_OFFSETS.at(m_Sample), frame);
                next(Phase::AwaitRestoreSample, 1);
                break;
            }
            release();
            if (m_End == 0)
            {
                m_End = 1;
                next(Phase::Maximize, STEP);
                break;
            }
            next(Phase::Stop, STEP);
            break;
        }
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    [[nodiscard]] static bool within(int errorX, int errorY) noexcept
    {
        return std::abs(errorX) <= TOLERANCE_PX && std::abs(errorY) <= TOLERANCE_PX;
    }

    /// One position checked; a miss is recorded only where SDL can place windows.
    void check(bool ok, std::string message)
    {
        if (!m_Observed.positionsApply)
        {
            return;
        }
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(std::move(message));
        }
    }

    /// The first x from one end at which a CLEAR_SPAN-wide run of the title bar at @p localY is clear
    /// of the icon and the buttons, plus half that run.
    [[nodiscard]] int clearPressX(int width, int localY, bool fromLeft) const
    {
        int clear = 0;
        for (int i = END_MARGIN; i < width / 2; ++i)
        {
            const int localX = fromLeft ? i : width - 1 - i;
            clear = m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY)) ? 0 : clear + 1;
            if (clear == CLEAR_SPAN)
            {
                return fromLeft ? localX - (CLEAR_SPAN / 2) : localX + (CLEAR_SPAN / 2);
            }
        }
        return width / 2;
    }

    /// A left-button press at window-local (@p localX, @p localY) of the window at (@p windowX, @p windowY).
    void press(int windowX, int windowY, int localX, int localY, int frame)
    {
        m_Press = {windowX + localX, windowY + localY};
        m_PointerShift = {0, 0};
        pushButton(static_cast<float>(localX), static_cast<float>(localY), true, 1);
        m_HeldAt = frame;
    }

    /// Whether a press with no effect is due to be made again (#2221): TitleBarLayer ends a drag or
    /// resize on SDL_EVENT_WINDOW_FOCUS_LOST, as it should, and under ctest -j another test's window
    /// mapping, or the maximize itself, can take the focus between the press and any step.
    [[nodiscard]] bool retryDue(int frame) const noexcept
    {
        return frame - m_HeldAt >= RETRY_EVERY;
    }

    /// The press made again on the window now at (@p windowX, @p windowY), where the pointer was at
    /// @p previous from the original press, then the pointer moved on to @p offset, within the
    /// original deadline. A drag goes by the pointer's movement from its press, so the window ends up
    /// where the original drag would have put it. A drag that restored the window can leave the
    /// pointer over the icon, a button or the top resize border of the restored one, where a press
    /// doesn't drag: then it is made in the middle of the title bar instead, and the pointer held that
    /// far from where it was from then on. The checks still measure from the original press.
    void
    retryPress(int windowX, int windowY, int width, bool maximized, std::pair<int, int> previous, std::pair<int, int> offset, int frame)
    {
        const int unshiftedLocalX = m_Press.first + previous.first - windowX;
        const int unshiftedLocalY = m_Press.second + previous.second - windowY;
        int localX = unshiftedLocalX + m_PointerShift.first;
        int localY = unshiftedLocalY + m_PointerShift.second;
        if (!maximized && (localY < TITLE_Y || m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY))))
        {
            localX = width / 2;
            localY = TITLE_Y;
        }
        m_PointerShift = {localX - unshiftedLocalX, localY - unshiftedLocalY};
        pushButton(static_cast<float>(localX), static_cast<float>(localY), true, 1);
        const int deadline = m_Deadline;
        holdAt(offset, frame);
        m_Deadline = deadline;
        ++m_Observed.pressRetries;
    }

    /// The pointer @p offset from the press with the left button down, until release(), waiting up to
    /// WAIT_LIMIT frames from @p frame for its effect.
    void holdAt(std::pair<int, int> offset, int frame)
    {
        m_HeldAt = frame;
        const int globalX = m_Press.first + m_PointerShift.first + offset.first;
        const int globalY = m_Press.second + m_PointerShift.second + offset.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
        m_Deadline = frame + WAIT_LIMIT;
    }

    /// The button up: the next update sees no button held and ends the interaction.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    static void pushButton(float x, float y, bool down, int clicks)
    {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = down;
        event.button.clicks = static_cast<Uint8>(clicks);
        event.button.x = x;
        event.button.y = y;
        static_cast<void>(SDL_PushEvent(&event));
    }

    enum class Phase : std::uint8_t
    {
        BeginPlainDrag,
        AwaitPlainDrag,
        BeginResize,
        AwaitResize,
        Maximize,
        AwaitMaximized,
        AwaitMaximizedSettled,
        CheckStillMaximized,
        AwaitRestoreSample,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int RETRY_EVERY = 60;    // frames before a press with no effect is made again
    static constexpr int SETTLED_FRAMES = 15; // frames a maximized window's geometry holds before the press
    static constexpr int MAX_FRAMES = 4000;
    static constexpr int TOLERANCE_PX = 2;
    static constexpr int TITLE_Y = 16;         // window-local y of a press: inside the title bar, below the resize border
    static constexpr int TITLE_Y_NEAR_TOP = 3; // a press near the top of a maximized window's title bar
    static constexpr int END_MARGIN = 4;       // px from the window's side where the search for a press starts
    static constexpr int CLEAR_SPAN = 16;
    static constexpr int WIDEN_BY = 60;
    static constexpr int INSIDE_THRESHOLD = App::DragTracker::DRAG_THRESHOLD_PX - 2;
    static constexpr std::array<std::pair<int, int>, 3> PLAIN_OFFSETS{{{40, 30}, {75, 55}, {-20, 90}}};
    static constexpr std::array<std::pair<int, int>, 4> RESTORE_OFFSETS{{{0, 40}, {15, 90}, {-30, 140}, {10, 180}}};

    DragGeometryObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::BeginPlainDrag;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::size_t m_Sample = 0;
    std::size_t m_End = 0; ///< 0: the press near the left end; 1: near the right
    std::pair<int, int> m_Start;
    int m_StartWidth = 0;
    std::pair<int, int> m_Press;        ///< Global: where the pointer was pressed, which the checks measure from
    std::pair<int, int> m_PointerShift; ///< How far the held pointer is from m_Press + offset after a lower re-press
    int m_HeldAt = 0;                   ///< The frame the pointer was last pressed or moved
    int m_PressLocalX = 0;
    int m_PressLocalY = 0;
    double m_Fraction = 0.0;
    std::array<int, 4> m_Geometry{}; ///< x, y, width, height on the last frame, while waiting for them to settle
    int m_StableFrames = 0;
};

// #2715: a plain title-bar drag follows the pointer 1:1, a right-edge resize keeps the left edge, and
// a drag that restores a maximized window keeps the pointer where it pressed on the title bar, across
// and down, pressed near either end.
TEST_F(UiLayersOnDisplayTest, TitleBarDragGeometryKeepsThePointerOnTheWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if (onNativeWayland())
    {
        GTEST_SKIP() << COMPOSITOR_DRAGS;
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayDragGeometryTest";
    spec.VSync = false;
    DragGeometryObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<DragGeometryDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    EXPECT_FALSE(observed.timedOut);
    EXPECT_TRUE(observed.widenedByResize);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    // #2716: the other path. The pushed presses above drove the client-side drag and resize whatever
    // the window's hit test says; this is what it says, so a real press at the same points takes the
    // OS's or the compositor's gesture where the backend runs it and the client-side one elsewhere.
    EXPECT_EQ(observed.plainPressHitTest, observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
    EXPECT_EQ(observed.resizePressHitTest, observed.systemResize ? SDL_HITTEST_RESIZE_RIGHT : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_EQ(observed.plainPressHitTest, SDL_HITTEST_DRAGGABLE); // Windows' own move loop: Aero Snap
    EXPECT_EQ(observed.resizePressHitTest, SDL_HITTEST_NORMAL);   // the resize stays the app's
#endif
    if (observed.maximizeUnavailable)
    {
        // The plain drag and the resize were checked above; only the restore part is skipped.
        GTEST_SKIP() << "The window never maximized on this display, so the drag-to-restore checks were skipped";
    }
    // A client-side maximized window (Windows) keeps the press for the restore under the pointer.
    const bool maximizedSystemDrag = App::computeSystemDragApplies(observed.systemDrag, observed.clientSideMaximize, /*isMaximized=*/true);
    for (std::size_t end = 0; end < 2; ++end)
    {
        SCOPED_TRACE(end == 0 ? "pressed near the left end" : "pressed near the right end");
        EXPECT_TRUE(observed.maximizedBeforeDrag.at(end));
        EXPECT_TRUE(observed.clickKeptMaximized.at(end)) << "a move inside the threshold restored the window";
        EXPECT_TRUE(observed.restoredByDrag.at(end));
        EXPECT_EQ(observed.maximizedPressHitTest.at(end), maximizedSystemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
#ifdef _WIN32
        EXPECT_EQ(observed.maximizedPressHitTest.at(end), SDL_HITTEST_NORMAL);
#endif
    }
    if (observed.positionsApply)
    {
        EXPECT_EQ(observed.checks, 12); // 3 plain-drag positions, the resize, 4 restore positions per end
    }
}

// ---- The live resize from every edge and corner (#2717) ----

} // namespace
