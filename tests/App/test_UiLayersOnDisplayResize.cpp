/// @file test_UiLayersOnDisplayResize.cpp
/// @brief UI::UILayer and App::TitleBarLayer in a real Core::Application (#1395): resizes from every edge and corner (#2717), a fast
/// resize, the hit test, the window menu (#3161) and the shell's window commands. Split from test_UiLayersOnDisplay.cpp by feature (#3850);
/// the shared checks and fixture are in UiLayersOnDisplayFixture.h.

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

constexpr int RESIZE_EDGE_INSET = 2;   // px inside a window edge: within the resize border at any display scale
constexpr int RESIZE_TOLERANCE_PX = 2; // how far a landed rectangle may be off, as the drag geometry test allows

/// Which of the window's edges a resize from @p edge pulls: the one pressed or, for a corner, both.
[[nodiscard]] constexpr bool pullsLeftEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Left || edge == App::ResizeEdge::TopLeft || edge == App::ResizeEdge::BottomLeft;
}

[[nodiscard]] constexpr bool pullsRightEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Right || edge == App::ResizeEdge::TopRight || edge == App::ResizeEdge::BottomRight;
}

[[nodiscard]] constexpr bool pullsTopEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Top || edge == App::ResizeEdge::TopLeft || edge == App::ResizeEdge::TopRight;
}

[[nodiscard]] constexpr bool pullsBottomEdge(App::ResizeEdge edge) noexcept
{
    return edge == App::ResizeEdge::Bottom || edge == App::ResizeEdge::BottomLeft || edge == App::ResizeEdge::BottomRight;
}

/// Whether a resize from @p edge moves the window's origin, which only a display that applies moves can show.
[[nodiscard]] constexpr bool movesTheWindowOrigin(App::ResizeEdge edge) noexcept
{
    return pullsLeftEdge(edge) || pullsTopEdge(edge);
}

/// Where the window should end up after a resize from @p edge of @p start with the pointer moved by
/// @p offset: the edge or corner pressed follows the pointer, held to the @p minimum size, and the
/// opposite edge or edges stay where they were.
[[nodiscard]] App::WindowRect
expectedResizeRect(App::ResizeEdge edge, const App::WindowRect& start, std::pair<int, int> offset, std::pair<int, int> minimum) noexcept
{
    int left = start.x;
    int top = start.y;
    int right = start.x + start.width;
    int bottom = start.y + start.height;
    if (pullsLeftEdge(edge))
    {
        left = std::min(start.x + offset.first, right - minimum.first);
    }
    if (pullsRightEdge(edge))
    {
        right = std::max(right + offset.first, start.x + minimum.first);
    }
    if (pullsTopEdge(edge))
    {
        top = std::min(start.y + offset.second, bottom - minimum.second);
    }
    if (pullsBottomEdge(edge))
    {
        bottom = std::max(bottom + offset.second, start.y + minimum.second);
    }
    return {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

/// The minimum a client-side resize holds the window to (TitleBarLayer::updateResize()): the one SDL
/// was handed, and never below Core::WINDOW_MIN_DIMENSION, as computeResizeGeometry() clamps it.
[[nodiscard]] std::pair<int, int> resizeMinimum(const Core::Window& window) noexcept
{
    const auto [width, height] = window.getMinimumSize();
    return {std::max(width, Core::WINDOW_MIN_DIMENSION), std::max(height, Core::WINDOW_MIN_DIMENSION)};
}

/// The window-local point to press for a resize from @p edge of a @p width x @p height window:
/// RESIZE_EDGE_INSET px inside the edge or corner, midway along an edge.
[[nodiscard]] std::pair<int, int> resizePressPoint(App::ResizeEdge edge, int width, int height) noexcept
{
    int x = width / 2;
    int y = height / 2;
    if (pullsLeftEdge(edge))
    {
        x = RESIZE_EDGE_INSET;
    }
    else if (pullsRightEdge(edge))
    {
        x = width - RESIZE_EDGE_INSET;
    }
    if (pullsTopEdge(edge))
    {
        y = RESIZE_EDGE_INSET;
    }
    else if (pullsBottomEdge(edge))
    {
        y = height - RESIZE_EDGE_INSET;
    }
    return {x, y};
}

[[nodiscard]] App::WindowRect currentRect(const Core::Window& window)
{
    const auto [width, height] = window.getSize();
    const auto [x, y] = window.getPosition();
    return {.x = x, .y = y, .width = width, .height = height};
}

[[nodiscard]] bool sameRect(const App::WindowRect& a, const App::WindowRect& b) noexcept
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

[[nodiscard]] bool withinRect(const App::WindowRect& a, const App::WindowRect& b) noexcept
{
    return std::abs(a.x - b.x) <= RESIZE_TOLERANCE_PX && std::abs(a.y - b.y) <= RESIZE_TOLERANCE_PX &&
           std::abs(a.width - b.width) <= RESIZE_TOLERANCE_PX && std::abs(a.height - b.height) <= RESIZE_TOLERANCE_PX;
}

[[nodiscard]] std::string describeRect(const App::WindowRect& rect)
{
    return std::format("{}x{} at ({}, {})", rect.width, rect.height, rect.x, rect.y);
}

/// A left-button press at window-local (@p localX, @p localY) of the test window.
void pushLeftButtonDown(int localX, int localY)
{
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.clicks = static_cast<Uint8>(1);
    event.button.x = static_cast<float>(localX);
    event.button.y = static_cast<float>(localY);
    static_cast<void>(SDL_PushEvent(&event));
}

/// What the edge-resize driver saw.
struct EdgeResizeObserved
{
    /// False under SDL's offscreen driver, which keeps a window where it was made, and on a desktop
    /// whose window manager ignored a direct setPosition() (#2707). CI always asserts.
    bool positionsApply = true;
    int checks = 0;  ///< Cases checked against where the window ended up
    int skipped = 0; ///< Cases that move the window's origin, skipped where positions don't apply
    std::vector<std::string> failures;
    int minimumWidth = 0;  ///< The minimum the resize held the window to...
    int minimumHeight = 0; ///< ...in window coordinates
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the one before had no effect (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: presses each window edge and corner in turn through the
/// title bar's test seams -- SDL_PushEvent() for the press, setMouseStateQueryForTesting() for the
/// held pointer -- and checks where the window lands (#2717): the edge or corner pressed follows the
/// pointer and the opposite edge or edges stay put; a pull past the minimum size stops there, with
/// the right or bottom edge still pinned when the left or top one was pulled. The top-right corner
/// included: the Close button's slot reaches it, but the corner square is the resize border's, not the
/// button's (#2882), so it resizes like the other three.
///
/// Each case waits for the window to reach its expected rectangle, up to WAIT_LIMIT frames (the size
/// commits at most every ResizeTracker::SIZE_COMMIT_INTERVAL_SECONDS, and an X11 window manager
/// applies a move some frames later), then records the miss. A resize whose window hasn't changed for
/// RETRY_EVERY frames was lost -- TitleBarLayer ends it on the focus loss another test's window
/// mapping over this one causes under `xvfb-run ctest -j` (#2221) -- so the press is made again at
/// the same edge of the window as it is now, the pointer still headed where it was: a resize goes by
/// the pointer's movement from the edge it pressed, so the window still ends up where expected.
class EdgeResizeDriverLayer : public Core::Layer
{
  public:
    EdgeResizeDriverLayer(EdgeResizeObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("EdgeResizeDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        const App::WindowRect now = currentRect(window);
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const auto nextCase = [this, &next]
        {
            next(++m_Case < CASES.size() ? Phase::Press : Phase::Stop, STEP);
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::ProbeMove:
        {
            // SDL's offscreen driver keeps a window where it was made, and WSLg's window manager
            // sometimes ignores client moves (#2707): ask for a move directly, and if it never lands,
            // skip the cases that move the window's origin. CI (TASKSMACK_REQUIRE_DISPLAY=1, no window
            // manager) asserts them all.
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            if (!m_Observed.positionsApply || TestSupport::displayRequired())
            {
                next(Phase::Press, STEP);
                break;
            }
            m_Start = now;
            window.setPosition(now.x + PROBE_X, now.y + PROBE_Y);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitProbeMove, 1);
            break;
        }
        case Phase::AwaitProbeMove:
            if (sameRect(now, m_Start) && waiting)
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = !sameRect(now, m_Start);
            next(Phase::Press, STEP);
            break;
        case Phase::Press:
        {
            const Case& resizeCase = CASES.at(m_Case);
            if (movesTheWindowOrigin(resizeCase.edge) && !m_Observed.positionsApply)
            {
                ++m_Observed.skipped;
                nextCase();
                break;
            }
            m_Start = now;
            m_Minimum = resizeMinimum(window);
            m_Offset = resizeCase.pastMinimum ? pastMinimumOffset(resizeCase.edge, now) : resizeCase.offset;
            m_Expected = expectedResizeRect(resizeCase.edge, now, m_Offset, m_Minimum);
            if (resizeCase.pastMinimum)
            {
                m_Observed.minimumWidth = m_Minimum.first;
                m_Observed.minimumHeight = m_Minimum.second;
            }
            const auto [localX, localY] = resizePressPoint(resizeCase.edge, now.width, now.height);
            // No press point is a control's: the top-right corner's is inside the Close button's slot,
            // which the corner square is taken back out of (#2882).
            if (m_TitleBar.isPointInControlArea(static_cast<float>(localX), static_cast<float>(localY)))
            {
                check(false, std::format("{}: the press point ({}, {}) is on a control", resizeCase.name, localX, localY));
                nextCase();
                break;
            }
            m_Target = {now.x + localX + m_Offset.first, now.y + localY + m_Offset.second};
            press(resizeCase.edge, now, frame);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitCase, 1);
            break;
        }
        case Phase::AwaitCase:
        {
            const Case& resizeCase = CASES.at(m_Case);
            const bool landed = withinRect(now, m_Expected);
            if (waiting && !landed)
            {
                if (!sameRect(now, m_LastSeen))
                {
                    m_LastSeen = now;
                    m_StillSince = frame;
                }
                else if (frame - m_StillSince >= RETRY_EVERY)
                {
                    press(resizeCase.edge, now, frame);
                    ++m_Observed.pressRetries;
                }
                next(Phase::AwaitCase, 1);
                break;
            }
            check(landed,
                  std::format("{}: pressed the {} window, pointer moved by ({}, {}), minimum {}x{}: expected {}, got {}",
                              resizeCase.name,
                              describeRect(m_Start),
                              m_Offset.first,
                              m_Offset.second,
                              m_Minimum.first,
                              m_Minimum.second,
                              describeRect(m_Expected),
                              describeRect(now)));
            release();
            nextCase();
            break;
        }
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    struct Case
    {
        std::string_view name;
        App::ResizeEdge edge;
        bool pastMinimum;           ///< The pointer moves by the window's whole size: the resize stops at the minimum
        std::pair<int, int> offset; ///< The pointer's movement from the press, when !pastMinimum
    };

    /// The pointer moved far enough into the window from @p edge to shrink it past any minimum.
    [[nodiscard]] static std::pair<int, int> pastMinimumOffset(App::ResizeEdge edge, const App::WindowRect& window) noexcept
    {
        int dx = 0;
        int dy = 0;
        if (pullsLeftEdge(edge))
        {
            dx = window.width;
        }
        else if (pullsRightEdge(edge))
        {
            dx = -window.width;
        }
        if (pullsTopEdge(edge))
        {
            dy = window.height;
        }
        else if (pullsBottomEdge(edge))
        {
            dy = -window.height;
        }
        return {dx, dy};
    }

    /// One case checked.
    void check(bool ok, std::string message)
    {
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(std::move(message));
        }
    }

    /// The press on @p edge of the window as it is now, @p window, with the pointer held at m_Target.
    void press(App::ResizeEdge edge, const App::WindowRect& window, int frame)
    {
        const auto [localX, localY] = resizePressPoint(edge, window.width, window.height);
        pushLeftButtonDown(localX, localY);
        const int globalX = m_Target.first;
        const int globalY = m_Target.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
        m_LastSeen = window;
        m_StillSince = frame;
    }

    /// The button up where it was: the next update sees no button held and ends the resize.
    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    enum class Phase : std::uint8_t
    {
        ProbeMove,
        AwaitProbeMove,
        Press,
        AwaitCase,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5;          // frames between cases: the release takes effect on the next one
    static constexpr int WAIT_LIMIT = 300;  // frames to wait for a rectangle before recording the miss
    static constexpr int RETRY_EVERY = 30;  // frames with no change before a press is made again
    static constexpr int MAX_FRAMES = 4000; // room for every wait to run out
    static constexpr int PROBE_X = 40;
    static constexpr int PROBE_Y = 30;
    static constexpr int RESIZE_BY = 60;
    // The pointer moves right and down by RESIZE_BY on every edge and corner: the left and top edges
    // shrink the window, the right and bottom ones grow it, and an edge ignores the movement across it.
    static constexpr std::array<Case, 11> CASES{{
        {.name = "left edge", .edge = App::ResizeEdge::Left, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "right edge", .edge = App::ResizeEdge::Right, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top edge", .edge = App::ResizeEdge::Top, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom edge", .edge = App::ResizeEdge::Bottom, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top-left corner", .edge = App::ResizeEdge::TopLeft, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "top-right corner", .edge = App::ResizeEdge::TopRight, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom-left corner", .edge = App::ResizeEdge::BottomLeft, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "bottom-right corner", .edge = App::ResizeEdge::BottomRight, .pastMinimum = false, .offset = {RESIZE_BY, RESIZE_BY}},
        {.name = "left edge past the minimum", .edge = App::ResizeEdge::Left, .pastMinimum = true, .offset = {}},
        {.name = "top edge past the minimum", .edge = App::ResizeEdge::Top, .pastMinimum = true, .offset = {}},
        {.name = "bottom-right corner past the minimum", .edge = App::ResizeEdge::BottomRight, .pastMinimum = true, .offset = {}},
    }};

  public:
    static constexpr int CASE_COUNT = static_cast<int>(CASES.size());

  private:
    EdgeResizeObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::ProbeMove;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::size_t m_Case = 0;
    App::WindowRect m_Start{};    ///< The window at the case's first press
    App::WindowRect m_Expected{}; ///< Where it should end up
    App::WindowRect m_LastSeen{}; ///< The window on the last frame, while waiting
    int m_StillSince = 0;         ///< The frame it last changed, or was pressed
    std::pair<int, int> m_Minimum;
    std::pair<int, int> m_Offset;
    std::pair<int, int> m_Target; ///< Global: where the pointer is held
};

// #2717: the resize the title bar runs client-side, from every edge and corner, live. The edge or
// corner pressed follows the pointer and the opposite edge or edges stay put; a pull past the
// minimum size stops there, the far edge still pinned. The top-right corner resizes like the other
// three although the Close button's slot reaches it: the corner square is the border's (#2882). The
// maths is unit-tested in ComputeResizeGeometryTest and ResizeTrackerTest; this checks the window on
// a display ends up where it says.
TEST_F(UiLayersOnDisplayTest, TitleBarResizesFromEveryEdgeAndCornerAndStopsAtTheMinimumSize)
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
    spec.Name = "UiLayersOnDisplayEdgeResizeTest";
    spec.VSync = false;
    EdgeResizeObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<EdgeResizeDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    RecordProperty("minimumWidth", observed.minimumWidth);
    RecordProperty("minimumHeight", observed.minimumHeight);
    RecordProperty("frames", observed.frames);
    EXPECT_FALSE(observed.timedOut);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_GE(observed.minimumWidth, Core::WINDOW_MIN_DIMENSION);
    EXPECT_GE(observed.minimumHeight, Core::WINDOW_MIN_DIMENSION);
    EXPECT_EQ(observed.checks + observed.skipped, EdgeResizeDriverLayer::CASE_COUNT);
    if (observed.positionsApply)
    {
        EXPECT_EQ(observed.skipped, 0);
    }
}

/// What the fast left-edge resize driver saw.
struct FastResizeObserved
{
    bool positionsApply = true; ///< As EdgeResizeObserved::positionsApply; a left-edge resize moves the window
    int checks = 0;             ///< Rectangles checked once the pointer stopped
    std::vector<std::string> failures;
    int rightEdgeAtPress = 0;
    /// Mid-sweep, before the held-back size commits, the moved origin shows with the old width
    /// (ResizeTracker holds the size back for SIZE_COMMIT_INTERVAL_SECONDS): how far off the right
    /// edge was at most, and on how many frames. Recorded, not asserted.
    int maxRightEdgeErrorWhileMoving = 0;
    int framesRightEdgeOffWhileMoving = 0;
    int frames = 0;
    int pressRetries = 0; ///< Presses made again because the resize was lost (#2221)
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: a fast resize from the left edge (#2717). The pointer is
/// pressed on the left edge and stepped STEP_PX on every frame, STEPS_PER_LEG frames into the window
/// and then the same back out, and after each leg, once the window has caught up, the right edge must
/// be where it was at the press and the top and bottom edges untouched: a resize goes by the pointer's
/// total movement from its press, so no step is lost or counted twice however fast the pointer moves.
/// The same once the button is let go. A lost resize is pressed again as EdgeResizeDriverLayer does.
class FastLeftResizeDriverLayer : public Core::Layer
{
  public:
    FastLeftResizeDriverLayer(FastResizeObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("FastLeftResizeDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        const App::WindowRect now = currentRect(window);
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::ProbeMove:
        {
            // As EdgeResizeDriverLayer: where moves don't land, the left edge can't be checked.
            const char* driver = SDL_GetCurrentVideoDriver();
            m_Observed.positionsApply = driver == nullptr || std::string_view(driver) != "offscreen";
            if (!m_Observed.positionsApply || TestSupport::displayRequired())
            {
                next(Phase::Press, STEP);
                break;
            }
            m_Start = now;
            window.setPosition(now.x + PROBE_X, now.y + PROBE_Y);
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitProbeMove, 1);
            break;
        }
        case Phase::AwaitProbeMove:
            if (sameRect(now, m_Start) && waiting)
            {
                next(Phase::AwaitProbeMove, 1);
                break;
            }
            m_Observed.positionsApply = !sameRect(now, m_Start);
            next(Phase::Press, STEP);
            break;
        case Phase::Press:
        {
            if (!m_Observed.positionsApply)
            {
                next(Phase::Stop, 1);
                break;
            }
            m_Start = now;
            m_Minimum = resizeMinimum(window);
            m_Observed.rightEdgeAtPress = now.x + now.width;
            const auto [localX, localY] = resizePressPoint(App::ResizeEdge::Left, now.width, now.height);
            m_Press = {now.x + localX, now.y + localY};
            m_Offset = 0;
            m_Leg = 0;
            m_StepsInLeg = 0;
            press(now, frame);
            next(Phase::Sweep, 1);
            break;
        }
        case Phase::Sweep:
        {
            // What the last step left: the origin moved at once, the size at most every commit interval.
            const int rightEdgeError = (now.x + now.width) - m_Observed.rightEdgeAtPress;
            m_Observed.maxRightEdgeErrorWhileMoving = std::max(m_Observed.maxRightEdgeErrorWhileMoving, std::abs(rightEdgeError));
            if (std::abs(rightEdgeError) > RESIZE_TOLERANCE_PX)
            {
                ++m_Observed.framesRightEdgeOffWhileMoving;
            }
            m_Offset += LEG_DIRECTIONS.at(m_Leg) * STEP_PX;
            hold();
            if (++m_StepsInLeg < STEPS_PER_LEG)
            {
                next(Phase::Sweep, 1);
                break;
            }
            m_Expected = expectedResizeRect(App::ResizeEdge::Left, m_Start, {m_Offset, 0}, m_Minimum);
            m_Deadline = frame + WAIT_LIMIT;
            m_LastSeen = now;
            m_StillSince = frame;
            next(Phase::AwaitLeg, 1);
            break;
        }
        case Phase::AwaitLeg:
        {
            const bool landed = withinRect(now, m_Expected);
            if (!landed && waiting)
            {
                if (!sameRect(now, m_LastSeen))
                {
                    m_LastSeen = now;
                    m_StillSince = frame;
                }
                else if (frame - m_StillSince >= RETRY_EVERY)
                {
                    press(now, frame);
                    ++m_Observed.pressRetries;
                }
                next(Phase::AwaitLeg, 1);
                break;
            }
            check(landed, std::format("after leg {} (pointer {} px from the press)", m_Leg + 1, m_Offset), now);
            if (++m_Leg < LEG_DIRECTIONS.size())
            {
                m_StepsInLeg = 0;
                next(Phase::Sweep, 1);
                break;
            }
            release();
            next(Phase::AfterRelease, STEP);
            break;
        }
        case Phase::AfterRelease:
            // Nothing was held back when the button went up: the window stays where the last leg left it.
            check(withinRect(now, m_Expected), "after the release", now);
            next(Phase::Stop, 1);
            break;
        case Phase::Stop:
            m_TitleBar.setMouseStateQueryForTesting({});
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// One rectangle checked once the pointer stopped.
    void check(bool ok, std::string_view when, const App::WindowRect& now)
    {
        ++m_Observed.checks;
        if (!ok)
        {
            m_Observed.failures.push_back(
                std::format("{}: pressed the left edge of the {} window; expected {}, got {} (right edge {} for {})",
                            when,
                            describeRect(m_Start),
                            describeRect(m_Expected),
                            describeRect(now),
                            now.x + now.width,
                            m_Observed.rightEdgeAtPress));
        }
    }

    /// The press on the left edge of the window as it is now, @p window, the pointer held where it is.
    void press(const App::WindowRect& window, int frame)
    {
        const auto [localX, localY] = resizePressPoint(App::ResizeEdge::Left, window.width, window.height);
        pushLeftButtonDown(localX, localY);
        hold();
        m_LastSeen = window;
        m_StillSince = frame;
    }

    /// The pointer m_Offset to the right of the press with the left button down, until release().
    void hold()
    {
        const int globalX = m_Press.first + m_Offset;
        const int globalY = m_Press.second;
        m_TitleBar.setMouseStateQueryForTesting([globalX, globalY]
                                                { return std::tuple<int, int, SDL_MouseButtonFlags>{globalX, globalY, SDL_BUTTON_LMASK}; });
    }

    void release()
    {
        m_TitleBar.setMouseStateQueryForTesting([] { return std::tuple<int, int, SDL_MouseButtonFlags>{0, 0, 0U}; });
    }

    enum class Phase : std::uint8_t
    {
        ProbeMove,
        AwaitProbeMove,
        Press,
        Sweep,
        AwaitLeg,
        AfterRelease,
        Stop,
    };

    static constexpr int SETTLE = 10;
    static constexpr int STEP = 5; // frames after the release before the check: it takes effect on the next one
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int RETRY_EVERY = 30;
    static constexpr int MAX_FRAMES = 1500; // room for every wait to run out
    static constexpr int PROBE_X = 40;
    static constexpr int PROBE_Y = 30;
    static constexpr int STEP_PX = 24;       // per frame: about 1,400 px/s at the loop's 60 fps
    static constexpr int STEPS_PER_LEG = 25; // 600 px into the window, then back out
    static constexpr std::array<int, 2> LEG_DIRECTIONS{1, -1};

    FastResizeObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::ProbeMove;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    App::WindowRect m_Start{};    ///< The window at the press
    App::WindowRect m_Expected{}; ///< Where the leg should leave it
    App::WindowRect m_LastSeen{}; ///< The window on the last frame, while waiting
    int m_StillSince = 0;
    std::pair<int, int> m_Minimum;
    std::pair<int, int> m_Press; ///< Global: where the pointer pressed
    int m_Offset = 0;            ///< How far right of the press the pointer is held
    std::size_t m_Leg = 0;
    int m_StepsInLeg = 0;

  public:
    static constexpr int CHECK_COUNT = static_cast<int>(LEG_DIRECTIONS.size()) + 1; ///< Each leg, then the release
};

// #2717: a fast resize from the left edge -- the pointer stepped STEP_PX on every frame, 600 px in
// and back out -- leaves the right edge where it was and the top and bottom edges untouched, after
// each leg and after the release: no drift.
TEST_F(UiLayersOnDisplayTest, AFastLeftEdgeResizeKeepsTheRightEdgeWhereItWas)
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
    spec.Name = "UiLayersOnDisplayFastResizeTest";
    spec.VSync = false;
    FastResizeObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<FastLeftResizeDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    RecordProperty("pressRetries", observed.pressRetries);
    RecordProperty("maxRightEdgeErrorWhileMoving", observed.maxRightEdgeErrorWhileMoving);
    RecordProperty("framesRightEdgeOffWhileMoving", observed.framesRightEdgeOffWhileMoving);
    RecordProperty("frames", observed.frames);
    EXPECT_FALSE(observed.timedOut);
    if (!observed.positionsApply)
    {
        GTEST_SKIP() << "This display doesn't apply window moves, so a left-edge resize can't be checked";
    }
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_EQ(observed.checks, FastLeftResizeDriverLayer::CHECK_COUNT);
}

/// What the hit-test driver saw (#2716).
struct HitTestObserved
{
    int checks = 0; ///< Points asked
    std::vector<std::string> failures;
    SDL_HitTestResult title = SDL_HITTEST_NORMAL;          ///< The empty title bar
    SDL_HitTestResult rightEdge = SDL_HITTEST_NORMAL;      ///< The right border, below the bar
    SDL_HitTestResult maximizedTitle = SDL_HITTEST_NORMAL; ///< The empty title bar of the maximized window
    bool systemDrag = false;                               ///< Core::VideoBackend::usesSystemDrag() while the app ran
    bool systemResize = false;                             ///< Core::VideoBackend::usesSystemResize()
    bool clientSideMaximize = false;                       ///< Core::VideoBackend::supportsClientSideMaximize()
    bool wayland = false;                                  ///< Core::VideoBackend::isWayland() while the app ran
    bool maximized = false;                                ///< The window maximized for the second round
    bool maximizeUnavailable = false;                      ///< It never did: that round was skipped
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: once the bar has drawn, asks the window's hit test --
/// TitleBarLayer::hitTest(), the function the callback installed on the real window forwards to --
/// what lies under the empty title bar, each button, the icon, every edge and corner and the content,
/// against what the backend's policy says it should answer; on Windows also asks the window itself
/// with WM_NCHITTEST, as Windows does when the pointer moves, so the installed callback and SDL's
/// mapping of its answers are covered too. Then maximizes the window and asks again: no resize
/// answers, and the title bar as the policy has it for a maximized window. Nothing is clicked or moved.
class HitTestDriverLayer : public Core::Layer
{
  public:
    HitTestDriverLayer(HitTestObserved& observed, App::TitleBarLayer& titleBar)
        : Layer("HitTestDriver"), m_Observed(observed), m_TitleBar(titleBar)
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
        Core::Window& window = Core::Application::get().getWindow();
        const auto [width, height] = window.getSize();
        const bool maximized = window.isMaximized();
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::AskNormal:
            m_Observed.systemDrag = Core::VideoBackend::usesSystemDrag();
            m_Observed.wayland = Core::VideoBackend::isWayland();
            m_Observed.systemResize = Core::VideoBackend::usesSystemResize();
            m_Observed.clientSideMaximize = Core::VideoBackend::supportsClientSideMaximize();
            askNormal(width, height);
            window.maximize();
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            if (!maximized)
            {
                m_Observed.maximizeUnavailable = true;
                next(Phase::Stop, 1);
                break;
            }
            m_Geometry = {};
            m_StableFrames = 0;
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximizedSettled, 1);
            break;
        case Phase::AwaitMaximizedSettled:
        {
            // The maximized flag can lead the window's new size by some frames (X11 reports the
            // configure later): wait until the geometry has held for SETTLED_FRAMES.
            const std::array<int, 2> geometry{width, height};
            m_StableFrames = geometry == m_Geometry ? m_StableFrames + 1 : 0;
            m_Geometry = geometry;
            if (m_StableFrames < SETTLED_FRAMES && waiting)
            {
                next(Phase::AwaitMaximizedSettled, 1);
                break;
            }
            m_Observed.maximized = true;
            askMaximized(width, height);
            window.restore();
            next(Phase::Stop, STEP);
            break;
        }
        case Phase::Stop:
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// One point asked: what the window answers for (@p x, @p y) against @p expected.
    SDL_HitTestResult check(const char* what, float x, float y, SDL_HitTestResult expected)
    {
        const SDL_HitTestResult got = m_TitleBar.hitTest(x, y);
        ++m_Observed.checks;
        if (got != expected)
        {
            m_Observed.failures.push_back(
                std::format("{} at ({}, {}): answered {}, expected {}", what, x, y, static_cast<int>(got), static_cast<int>(expected)));
        }
        return got;
    }

    void askNormal(int width, int height)
    {
        const App::WindowHitTestInput input = m_TitleBar.hitTestInput();
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        const float barMiddleY = input.titleBarHeightPx * 0.5F;
        const SDL_HitTestResult drag = m_Observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
        const auto resize = [this](SDL_HitTestResult edge)
        {
            return m_Observed.systemResize ? edge : SDL_HITTEST_NORMAL;
        };

        m_Observed.title = check("the empty title bar", w * 0.5F, barMiddleY, drag);
        check("the gap before the window controls", input.buttons.minimize.minX - 1.0F, barMiddleY, drag);
        const auto& b = input.buttons;
        const std::array<std::pair<const char*, const App::ButtonBounds*>, 6> buttons{{{"About", &b.about},
                                                                                       {"Help", &b.help},
                                                                                       {"Settings", &b.settings},
                                                                                       {"Minimize", &b.minimize},
                                                                                       {"Maximize", &b.maximize},
                                                                                       {"Close", &b.close}}};
        for (const auto& [name, bounds] : buttons)
        {
            check(name, (bounds->minX + bounds->maxX) * 0.5F, (bounds->minY + bounds->maxY) * 0.5F, SDL_HITTEST_NORMAL);
        }
        if (input.icon.maxX > input.icon.minX)
        {
            check("the icon", (input.icon.minX + input.icon.maxX) * 0.5F, (input.icon.minY + input.icon.maxY) * 0.5F, SDL_HITTEST_NORMAL);
        }
        check("the left edge", 1.0F, h * 0.5F, resize(SDL_HITTEST_RESIZE_LEFT));
        m_Observed.rightEdge = check("the right edge", w - 2.0F, h * 0.5F, resize(SDL_HITTEST_RESIZE_RIGHT));
        check("the top edge", w * 0.5F, 1.0F, resize(SDL_HITTEST_RESIZE_TOP));
        check("the bottom edge", w * 0.5F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOM));
        check("the top-left corner", 1.0F, 1.0F, resize(SDL_HITTEST_RESIZE_TOPLEFT));
        check("the bottom-left corner", 1.0F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOMLEFT));
        check("the bottom-right corner", w - 2.0F, h - 2.0F, resize(SDL_HITTEST_RESIZE_BOTTOMRIGHT));
        // The top-right corner is the border's although the Close button's slot reaches it (#2882); one
        // pixel past the corner square, left of it or below it, is the button.
        const float border = input.resizeBorderPx;
        check("the top-right corner", w - 2.0F, 1.0F, resize(SDL_HITTEST_RESIZE_TOPRIGHT));
        check("the top-right corner's innermost pixel", w - border, border - 1.0F, resize(SDL_HITTEST_RESIZE_TOPRIGHT));
        check("the close button left of the corner", w - border - 1.0F, border - 1.0F, SDL_HITTEST_NORMAL);
        check("the close button below the corner", w - 2.0F, border, SDL_HITTEST_NORMAL);
        check("the content", w * 0.5F, input.titleBarHeightPx + 50.0F, SDL_HITTEST_NORMAL);
#ifdef _WIN32
        askWindows(width, height, b.close, barMiddleY, drag, resize(SDL_HITTEST_RESIZE_RIGHT), resize(SDL_HITTEST_RESIZE_TOPRIGHT));
#endif
    }

    void askMaximized(int width, int height)
    {
        const App::WindowHitTestInput input = m_TitleBar.hitTestInput();
        const auto w = static_cast<float>(width);
        const auto h = static_cast<float>(height);
        const SDL_HitTestResult drag =
            App::computeSystemDragApplies(m_Observed.systemDrag, m_Observed.clientSideMaximize, /*isMaximized=*/true)
                ? SDL_HITTEST_DRAGGABLE
                : SDL_HITTEST_NORMAL;
        m_Observed.maximizedTitle = check("the maximized window's empty title bar", w * 0.5F, input.titleBarHeightPx * 0.5F, drag);
        check("the maximized window's top strip", w * 0.5F, 1.0F, drag); // no resize border: the bar
        check("the maximized window's right edge", w - 2.0F, h * 0.5F, SDL_HITTEST_NORMAL);
        check("the maximized window's bottom-left corner", 1.0F, h - 2.0F, SDL_HITTEST_NORMAL);
        check("the maximized window's close button", w - 2.0F, 1.0F, SDL_HITTEST_NORMAL);
    }

#ifdef _WIN32
    /// Asks the window itself, as Windows does when the pointer moves: the window procedure hands
    /// WM_NCHITTEST to the installed callback, and SDL maps its answer to a Win32 hit-test code --
    /// DRAGGABLE is HTCAPTION, which is what Aero Snap needs. Only a question: nothing is clicked.
    void askWindows(int width,
                    int height,
                    const App::ButtonBounds& close,
                    float barMiddleY,
                    SDL_HitTestResult title,
                    SDL_HitTestResult rightEdge,
                    SDL_HitTestResult topRightCorner)
    {
        const SDL_PropertiesID props = SDL_GetWindowProperties(Core::Application::get().getWindow().getHandle());
        auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd == nullptr)
        {
            m_Observed.failures.emplace_back("no HWND for the window");
            return;
        }
        const auto ask = [hwnd](float x, float y)
        {
            POINT point{.x = static_cast<LONG>(x), .y = static_cast<LONG>(y)};
            ClientToScreen(hwnd, &point);
            return SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y));
        };
        const auto code = [](SDL_HitTestResult result) -> LRESULT
        {
            switch (result)
            {
            case SDL_HITTEST_DRAGGABLE:
                return HTCAPTION;
            case SDL_HITTEST_RESIZE_RIGHT:
                return HTRIGHT;
            case SDL_HITTEST_RESIZE_TOPRIGHT:
                return HTTOPRIGHT;
            default:
                return HTCLIENT;
            }
        };
        const auto checkWindows = [this, &ask, &code](const char* what, float x, float y, SDL_HitTestResult expected)
        {
            const LRESULT got = ask(x, y);
            ++m_Observed.checks;
            if (got != code(expected))
            {
                m_Observed.failures.push_back(
                    std::format("WM_NCHITTEST on {} at ({}, {}): answered {}, expected {}", what, x, y, got, code(expected)));
            }
        };
        checkWindows("the empty title bar", static_cast<float>(width) * 0.5F, barMiddleY, title);
        checkWindows("the Close button", (close.minX + close.maxX) * 0.5F, (close.minY + close.maxY) * 0.5F, SDL_HITTEST_NORMAL);
        checkWindows("the right edge", static_cast<float>(width) - 2.0F, static_cast<float>(height) * 0.5F, rightEdge);
        // The corner the Close button's slot reaches (#2882): the border's, so on Windows HTCLIENT like
        // every border, and the press comes to the app's resize rather than the OS's modal one.
        checkWindows("the top-right corner", static_cast<float>(width) - 2.0F, 1.0F, topRightCorner);
    }
#endif

    enum class Phase : std::uint8_t
    {
        AskNormal,
        AwaitMaximized,
        AwaitMaximizedSettled,
        Stop,
    };

    static constexpr int SETTLE = 10; // frames before the first question: the bar has drawn and cached its layout
    static constexpr int STEP = 10;
    static constexpr int WAIT_LIMIT = 300;
    static constexpr int SETTLED_FRAMES = 15;
    static constexpr int MAX_FRAMES = 900;

    HitTestObserved& m_Observed;
    App::TitleBarLayer& m_TitleBar;
    Phase m_Phase = Phase::AskNormal;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    std::array<int, 2> m_Geometry{};
    int m_StableFrames = 0;
};

// #2716: the hit-test callback TitleBarLayer installs on the real window answers for the title bar, the
// buttons and the edges as the backend's policy says: on Windows the empty title bar is DRAGGABLE
// (HTCAPTION to Windows, so its own move loop and Aero Snap run the drag) and the edges stay the app's
// resize; on X11 the edges are the window manager's and the bar the app's; on native Wayland both are
// the compositor's. Maximized, nothing is a resize edge.
TEST_F(UiLayersOnDisplayTest, TheHitTestCallbackAnswersForTheRealWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayHitTestTest";
    spec.VSync = false;
    HitTestObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        auto& titleBar = app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<HitTestDriverLayer>(observed, titleBar);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    for (const std::string& failure : observed.failures)
    {
        ADD_FAILURE() << failure;
    }
    EXPECT_GE(observed.checks, 17); // the bar, the gap, 6 buttons, 7 edges and corners, the close corner, the content
    EXPECT_EQ(observed.title, observed.systemDrag ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL);
    EXPECT_EQ(observed.rightEdge, observed.systemResize ? SDL_HITTEST_RESIZE_RIGHT : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_TRUE(observed.systemDrag);
    EXPECT_FALSE(observed.systemResize);
    EXPECT_EQ(observed.title, SDL_HITTEST_DRAGGABLE);
    EXPECT_EQ(observed.rightEdge, SDL_HITTEST_NORMAL);
#endif
    if (observed.wayland)
    {
        // Native Wayland hands the compositor both (#744): what the drag tests can't drive there (#2724).
        EXPECT_EQ(observed.title, SDL_HITTEST_DRAGGABLE);
        EXPECT_EQ(observed.rightEdge, SDL_HITTEST_RESIZE_RIGHT);
    }
    if (observed.maximizeUnavailable)
    {
        GTEST_SKIP() << "The window never maximized on this display, so the maximized round was skipped";
    }
    EXPECT_TRUE(observed.maximized);
    EXPECT_EQ(observed.maximizedTitle,
              App::computeSystemDragApplies(observed.systemDrag, observed.clientSideMaximize, /*isMaximized=*/true) ? SDL_HITTEST_DRAGGABLE
                                                                                                                    : SDL_HITTEST_NORMAL);
#ifdef _WIN32
    EXPECT_EQ(observed.maximizedTitle, SDL_HITTEST_NORMAL); // the restore under the pointer stays the app's (#2715)
#endif
}

/// What the system-menu driver saw.
struct SystemMenuObserved
{
    App::WindowRect normal{};         ///< The window before it was maximized
    bool maximizeUnavailable = false; ///< The window never maximized: the menu's checks were skipped
    bool openedOnMaximized = false;   ///< Ctrl+Space opened the menu on the maximized window
    bool restoredByMenu = false;      ///< Its Restore restored the window...
    App::WindowRect restored{};       ///< ...to this rectangle
    bool openedOnNormal = false;      ///< Ctrl+Space opened the menu on the restored window
    bool maximizedByMenu = false;     ///< Its Maximize maximized the window
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: maximizes the window, opens the window menu with Ctrl+Space
/// as a user does (TitleBarLayer::onSDLEvent()), chooses Restore and waits for the window to be back at
/// its normal rectangle; then opens the menu again and chooses Maximize (#3161). Each wait is up to
/// WAIT_LIMIT frames: the window manager may apply either some frames later (#2054).
class SystemMenuDriverLayer : public Core::Layer
{
  public:
    explicit SystemMenuDriverLayer(SystemMenuObserved& observed) : Layer("SystemMenuDriver"), m_Observed(observed)
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
        Core::Window& window = Core::Application::get().getWindow();
        const bool maximized = window.isMaximized();
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        const bool waiting = frame < m_Deadline;
        switch (m_Phase)
        {
        case Phase::Maximize:
            m_Observed.normal = currentRect(window);
            window.maximize();
            m_Deadline = frame + WAIT_LIMIT;
            next(Phase::AwaitMaximized, 1);
            break;
        case Phase::AwaitMaximized:
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximized, 1);
                break;
            }
            if (!maximized)
            {
                m_Observed.maximizeUnavailable = true;
                next(Phase::Stop, 1);
                break;
            }
            pushCtrlSpace();
            next(Phase::ChooseRestore, STEP);
            break;
        case Phase::ChooseRestore:
            m_Observed.openedOnMaximized = choose(ICON_FA_WINDOW_RESTORE "  Restore");
            m_Deadline = frame + WAIT_LIMIT;
            m_StableFrames = 0;
            next(m_Observed.openedOnMaximized ? Phase::AwaitRestored : Phase::Stop, 1);
            break;
        case Phase::AwaitRestored:
        {
            // On X11 the restore is a request: wait until the window is restored and its geometry has
            // held for STABLE_FRAMES, so the rectangle read is where it settled.
            const App::WindowRect rect = currentRect(window);
            m_StableFrames = (!maximized && sameRect(rect, m_Observed.restored)) ? m_StableFrames + 1 : 0;
            m_Observed.restored = rect;
            if (m_StableFrames < STABLE_FRAMES && waiting)
            {
                next(Phase::AwaitRestored, 1);
                break;
            }
            m_Observed.restoredByMenu = !maximized;
            pushCtrlSpace();
            next(Phase::ChooseMaximize, STEP);
            break;
        }
        case Phase::ChooseMaximize:
            m_Observed.openedOnNormal = choose(ICON_FA_WINDOW_MAXIMIZE "  Maximize");
            m_Deadline = frame + WAIT_LIMIT;
            next(m_Observed.openedOnNormal ? Phase::AwaitMaximizedByMenu : Phase::Stop, 1);
            break;
        case Phase::AwaitMaximizedByMenu:
            m_Observed.maximizedByMenu = maximized;
            if (!maximized && waiting)
            {
                next(Phase::AwaitMaximizedByMenu, 1);
                break;
            }
            next(Phase::Stop, 1);
            break;
        case Phase::Stop:
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// Ctrl+Space, through SDL's queue as a real key press arrives: TitleBarLayer opens the window menu.
    static void pushCtrlSpace()
    {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
        event.key.key = SDLK_SPACE;
        event.key.scancode = SDL_SCANCODE_SPACE; // as a real key event carries; ImGui's SDL backend expects one
        event.key.mod = SDL_KMOD_LCTRL;
        event.key.down = true;
        static_cast<void>(SDL_PushEvent(&event));
    }

    /// Choose the open window menu's item @p label, which the menu reports the next frame. False when
    /// the menu isn't open.
    static bool choose(const char* label)
    {
        if (GImGui->OpenPopupStack.empty() || GImGui->OpenPopupStack.back().Window == nullptr)
        {
            return false;
        }
        ImGui::ActivateItemByID(ImHashStr(label, 0, GImGui->OpenPopupStack.back().Window->ID));
        return true;
    }

    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        Maximize,
        AwaitMaximized,
        ChooseRestore,
        AwaitRestored,
        ChooseMaximize,
        AwaitMaximizedByMenu,
        Stop,
    };

    static constexpr int SETTLE = 10;       // frames before the first step: fonts, layout and minimum size settle
    static constexpr int STEP = 10;         // frames for a key press to open the menu
    static constexpr int STABLE_FRAMES = 5; // frames the restored window must hold still
    static constexpr int WAIT_LIMIT = 300;  // frames each maximize and restore may take
    static constexpr int MAX_FRAMES = 1200;

    SystemMenuObserved& m_Observed;
    Phase m_Phase = Phase::Maximize;
    int m_NextFrame = SETTLE;
    int m_Deadline = 0;
    int m_StableFrames = 0;
};

// #3161: the window menu's Restore restores a maximized window to the rectangle it had, and its
// Maximize maximizes a normal one, as the title-bar buttons do. The menu is TaskSmack's own, opened by
// Ctrl+Space, on every platform; the native Windows menu the icon opens maps its choice to the same
// actions (TitleBarSystemMenuTest), and its Restore and Maximize sent as WM_SYSCOMMAND are covered by
// TheShellsRestoreAndMaximizeAreTheClientSideOnes.
TEST_F(UiLayersOnDisplayTest, TheWindowMenusRestoreAndMaximizeDoWhatTheySay)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }
    if (onNativeWayland() && !compositorMaximizes())
    {
        GTEST_SKIP() << "native Wayland: this compositor doesn't maximize the window, so there is nothing to restore";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplaySystemMenuTest";
    spec.VSync = false;
    SystemMenuObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<SystemMenuDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    if (observed.maximizeUnavailable)
    {
        GTEST_SKIP() << "The window never maximized on this display, so the window menu's Restore can't be checked";
    }
    ASSERT_TRUE(observed.openedOnMaximized) << "Ctrl+Space didn't open the window menu";
    EXPECT_TRUE(observed.restoredByMenu) << "the window menu's Restore left the window maximized";
    // Native Wayland never tells TaskSmack where the window is; elsewhere it is back where it was.
    if (!onNativeWayland())
    {
        EXPECT_TRUE(withinRect(observed.restored, observed.normal))
            << "restored to " << describeRect(observed.restored) << ", was " << describeRect(observed.normal);
#ifdef _WIN32
        EXPECT_TRUE(sameRect(observed.restored, observed.normal)) // the client-side restore is exact
            << "restored to " << describeRect(observed.restored) << ", was " << describeRect(observed.normal);
#endif
    }
    ASSERT_TRUE(observed.openedOnNormal) << "Ctrl+Space didn't open the window menu again";
    EXPECT_TRUE(observed.maximizedByMenu) << "the window menu's Maximize didn't maximize the window";
}

#ifdef _WIN32
/// What the shell-command driver saw.
struct ShellCommandObserved
{
    App::WindowRect normal{};         ///< The window before it was maximized
    bool maximizeUnavailable = false; ///< The window never maximized: the checks were skipped
    bool restoredAtOnce = false;      ///< WM_SYSCOMMAND SC_RESTORE restored it before SendMessage returned...
    App::WindowRect restored{};       ///< ...to this rectangle
    bool stayedRestored = false;      ///< and it was still restored once the window's events were handled
    bool maximizedAtOnce = false;     ///< SC_MAXIMIZE maximized the normal window client-side at once:
    bool zoomedByWindows = true;      ///< not an OS maximize (IsZoomed()) the window would have to adopt (#1208)
    App::WindowRect maximized{};      ///< The rectangle SC_MAXIMIZE gave it
    std::pair<int, int> usable{};     ///< The usable bounds of its display
    bool stayedMaximized = false;     ///< Still maximized client-side once the events were handled
    bool minimizedByShell = false;    ///< SC_MINIMIZE, with no Win+Down, minimized the maximized window...
    bool backMaximized = false;       ///< ...and the taskbar's SC_RESTORE brought it back maximized, not restored
    App::WindowRect restoredAgain{};  ///< Where SC_RESTORE then put it: the rectangle from before SC_MAXIMIZE
    int frames = 0;
    bool timedOut = false;
};

/// Pushed after UILayer and TitleBarLayer: sends the real window the shell's window commands, as the
/// native system menu (posted to it before #3161), the taskbar and other programs do -- WM_SYSCOMMAND
/// to this window alone, through its subclass procedure -- and records what each did: SC_RESTORE on a
/// client-side maximized window, SC_MAXIMIZE on a normal one, then SC_MINIMIZE and the taskbar's
/// SC_RESTORE on the maximized one, and SC_RESTORE once more. Each is carried out before SendMessage
/// returns, so it is read at once and again STEP frames later, after the window's events.
class ShellCommandDriverLayer : public Core::Layer
{
  public:
    explicit ShellCommandDriverLayer(ShellCommandObserved& observed) : Layer("ShellCommandDriver"), m_Observed(observed)
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
        Core::Window& window = Core::Application::get().getWindow();
        auto* const hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(window.getHandle()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd == nullptr)
        {
            ADD_FAILURE() << "the window has no HWND";
            Core::Application::get().stop();
            return;
        }
        const auto next = [this, frame](Phase phase, int frames)
        {
            m_Phase = phase;
            m_NextFrame = frame + frames;
        };
        switch (m_Phase)
        {
        case Phase::Maximize:
            // No Win+Down, whatever the real keyboard is doing: SC_MINIMIZE below must minimize (#1279).
            window.setShellRestoreKeysReader([] noexcept { return std::pair{false, false}; });
            m_Observed.normal = currentRect(window);
            window.maximize(); // client-side, as the title-bar button does
            next(Phase::Restore, STEP);
            break;
        case Phase::Restore:
            if (!window.isMaximized())
            {
                m_Observed.maximizeUnavailable = true;
                Core::Application::get().stop();
                break;
            }
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            m_Observed.restoredAtOnce = !window.isMaximized();
            m_Observed.restored = currentRect(window);
            next(Phase::MaximizeNormal, STEP);
            break;
        case Phase::MaximizeNormal:
            m_Observed.stayedRestored = !window.isMaximized();
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            m_Observed.maximizedAtOnce = window.isMaximized();
            m_Observed.zoomedByWindows = IsZoomed(hwnd) != FALSE;
            m_Observed.maximized = currentRect(window);
            m_Observed.usable = window.getUsableDisplaySize().value_or(std::pair{0, 0});
            next(Phase::MinimizeAndBack, STEP);
            break;
        case Phase::MinimizeAndBack:
            m_Observed.stayedMaximized = window.isMaximized() && IsZoomed(hwnd) == FALSE;
            // Both in this frame: a minimized window gets no frames to render in.
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            m_Observed.minimizedByShell = window.isMinimized();
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            m_Observed.backMaximized = !window.isMinimized() && window.isMaximized();
            next(Phase::RestoreAgain, STEP);
            break;
        case Phase::RestoreAgain:
            SendMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            m_Observed.restoredAgain = currentRect(window);
            next(Phase::Stop, STEP);
            break;
        case Phase::Stop:
            Core::Application::get().stop();
            break;
        }
    }

  private:
    /// The steps, in order; each waits m_NextFrame before running.
    enum class Phase : std::uint8_t
    {
        Maximize,
        Restore,
        MaximizeNormal,
        MinimizeAndBack,
        RestoreAgain,
        Stop,
    };

    static constexpr int SETTLE = 10; // frames before the first step: fonts, layout and minimum size settle
    static constexpr int STEP = 10;   // frames between commands, for the window's events to be handled
    static constexpr int MAX_FRAMES = 600;

    ShellCommandObserved& m_Observed;
    Phase m_Phase = Phase::Maximize;
    int m_NextFrame = SETTLE;
};

// #3161: WM_SYSCOMMAND SC_RESTORE and SC_MAXIMIZE reach the client-side maximize. Windows never
// considers the client-side maximized window maximized, so it ignored the native system menu's
// Restore, and carried out its Maximize as an OS maximize. The window's subclass procedure now carries
// both out as the title-bar buttons do, while the taskbar's Restore of a minimized window is still
// Windows': the window comes back as it was, maximized.
TEST_F(UiLayersOnDisplayTest, TheShellsRestoreAndMaximizeAreTheClientSideOnes)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "UiLayersOnDisplayShellCommandTest";
    spec.VSync = false;
    ShellCommandObserved observed;
    try
    {
        Core::Application app(spec);
        app.pushLayer<UI::UILayer>();
        app.pushLayer<App::TitleBarLayer>();
        app.pushLayer<ShellCommandDriverLayer>(observed);
        app.run();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application failed after the display probe passed: " << e.what();
    }

    EXPECT_FALSE(observed.timedOut);
    if (observed.maximizeUnavailable)
    {
        GTEST_SKIP() << "Maximize unavailable on this display (headless environment)";
    }
    EXPECT_TRUE(observed.restoredAtOnce) << "SC_RESTORE left the client-side maximized window maximized";
    EXPECT_TRUE(sameRect(observed.restored, observed.normal))
        << "restored to " << describeRect(observed.restored) << ", was " << describeRect(observed.normal);
    EXPECT_TRUE(observed.stayedRestored);

    EXPECT_TRUE(observed.maximizedAtOnce) << "SC_MAXIMIZE didn't maximize the window client-side";
    EXPECT_FALSE(observed.zoomedByWindows) << "SC_MAXIMIZE was carried out as an OS maximize";
    EXPECT_EQ(std::pair(observed.maximized.width, observed.maximized.height), observed.usable);
    EXPECT_TRUE(observed.stayedMaximized);

    EXPECT_TRUE(observed.minimizedByShell);
    EXPECT_TRUE(observed.backMaximized) << "the taskbar's SC_RESTORE of the minimized window didn't bring it back maximized";
    EXPECT_TRUE(sameRect(observed.restoredAgain, observed.normal))
        << "restored to " << describeRect(observed.restoredAgain) << ", was " << describeRect(observed.normal);
}
#endif

} // namespace
