/// @file test_Application.cpp
/// @brief Tests for Core::Application lifecycle and layer management
///
/// Tests cover:
/// - Application construction and initialization
/// - Layer stack management (push, lifecycle callbacks)
/// - Application run/stop control
/// - Singleton instance access
/// - Error handling (SDL initialization)
///
/// Note: The ApplicationTest suite requires a display/windowing system with a GL 3.3 core context and
/// is skipped when the up-front display probe finds none. Once the probe passes, a construction
/// exception is a test failure, not a skip; and with TASKSMACK_REQUIRE_DISPLAY=1 (Linux CI) a failed
/// probe fails too (#1132). The FramePacingTest suite is pure logic extracted from Application::run() (see
/// Core/FramePacing.h) and always runs, headless or not.

#include "Core/AnimationRequest.h"
#include "Core/Application.h"
#include "Core/Event.h"
#include "Core/FramePacing.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/PathService.h"
#include "Core/ResizePerfOperation.h"
#include "Core/ResizePerfTrace.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace
{

// Check if we have a display available
bool detectDisplay()
{
#ifdef _WIN32
    // Check for CI environment - GitHub Actions sets CI=true
    char* ciEnv = nullptr;
    size_t len = 0;
    _dupenv_s(&ciEnv, &len, "CI");
    bool isCI = (ciEnv != nullptr && std::string(ciEnv) == "true");
    free(ciEnv);

    if (isCI)
    {
        // Windows CI runners are typically headless
        return false;
    }
    // Local Windows development usually has a display
    return true;
#else
    // On Linux, check for DISPLAY environment variable (X11) or WAYLAND_DISPLAY
    // NOLINTBEGIN(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay) - called during single-threaded test startup, read-only env check
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    // NOLINTEND(concurrency-mt-unsafe, cppcoreguidelines-pro-bounds-array-to-pointer-decay)
    if ((display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0'))
    {
        // Xvfb and other virtual displays expose DISPLAY but may lack a usable
        // GL 3.3 core stack. Probe capability before committing to the real-display
        // path; if the probe fails, fall through to the offscreen fallback so both
        // Application and Window test suites behave consistently.
        if (TestSupport::probeGLCapability())
        {
            return true;
        }
    }

    // The offscreen driver only counts if it can also create a GL 3.3 core context (it needs
    // Mesa EGL for that), so a "yes" here means Application construction can succeed and a
    // construction exception is a real failure, not an environment gap.
    return TestSupport::tryEnableOffscreenVideoDriver() && TestSupport::probeGLCapability();
#endif
}

// Every display check goes through here so TASKSMACK_REQUIRE_DISPLAY=1 (set by Linux CI) turns a
// missing display into a failure instead of a skip.
bool hasDisplay()
{
    return TestSupport::enforceDisplayRequirement(detectDisplay());
}

/// Test layer that tracks lifecycle callbacks
class TestLayer : public Core::Layer
{
  public:
    explicit TestLayer(const std::string& name = "TestLayer") : Layer(name)
    {}

    void onAttach() override
    {
        attachCalled = true;
    }

    void onDetach() override
    {
        detachCalled = true;
    }

    void onUpdate(float deltaTime) override
    {
        updateCalled = true;
        lastDeltaTime = deltaTime;
        updateCount++;
    }

    void onRender() override
    {
        renderCalled = true;
        renderCount++;
    }

    void onPostRender() override
    {
        postRenderCalled = true;
    }

    bool attachCalled = false;
    bool detachCalled = false;
    bool updateCalled = false;
    bool renderCalled = false;
    bool postRenderCalled = false;
    float lastDeltaTime = 0.0F;
    int updateCount = 0;
    int renderCount = 0;
};

/// Layer that requests app stop after N updates
class StopAfterNLayer : public Core::Layer
{
  public:
    explicit StopAfterNLayer(int n) : Layer("StopLayer"), m_MaxUpdates(n)
    {}

    void onUpdate(float /* deltaTime */) override
    {
        m_UpdateCount++;
        if (m_UpdateCount >= m_MaxUpdates)
        {
            Core::Application::get().stop();
        }
    }

  private:
    int m_MaxUpdates;
    int m_UpdateCount = 0;
};

/// Layer that records which events it received and optionally marks them handled
class EventTrackingLayer : public Core::Layer
{
  public:
    /// @param name         Layer name.
    /// @param handleEvents If true, marks every received event as handled.
    /// @param dispatchLog  Optional shared vector; when non-null, appends the
    ///                     layer name on each received event so callers can
    ///                     assert dispatch order across multiple layers.
    explicit EventTrackingLayer(const std::string& name, bool handleEvents = false, std::vector<std::string>* dispatchLog = nullptr)
        : Layer(name), m_HandleEvents(handleEvents), m_DispatchLog(dispatchLog)
    {}

    void onEvent(Core::Event& event) override
    {
        receivedEventNames.push_back(event.getName());
        if (m_DispatchLog != nullptr)
        {
            m_DispatchLog->push_back(getName());
        }
        if (m_HandleEvents)
        {
            event.setHandled(true);
        }
    }

    std::vector<std::string> receivedEventNames;

  private:
    bool m_HandleEvents;
    std::vector<std::string>* m_DispatchLog;
};

/// Layer whose onEvent() always throws, to verify raiseEvent() can't be crashed by a
/// misbehaving layer (#778) and that dispatch continues to the remaining layers afterward.
class ThrowingLayer : public Core::Layer
{
  public:
    explicit ThrowingLayer(const std::string& name) : Layer(name)
    {}

    void onEvent(Core::Event& /*event*/) override
    {
        throw std::runtime_error("ThrowingLayer::onEvent always throws");
    }
};

/// Layer whose onAttach() always throws, to verify pushLayer() pops a half-initialized
/// layer back off the stack instead of leaving it there (#779). onDetach() logs to
/// g_DetachOrder like TrackedLayer so a test can assert it was never called.
class ThrowingOnAttachLayer : public Core::Layer
{
  public:
    explicit ThrowingOnAttachLayer(const std::string& name) : Layer(name)
    {}

    void onAttach() override
    {
        throw std::runtime_error("ThrowingOnAttachLayer::onAttach always throws");
    }

    void onDetach() override;
};

/// Layer that handles WindowCloseEvent, returning `veto` from its handler, and counts how many it saw.
class CloseListenerLayer : public Core::Layer
{
  public:
    CloseListenerLayer(const std::string& name, bool veto) : Layer(name), m_Veto(veto)
    {}

    void onEvent(Core::Event& event) override
    {
        Core::EventDispatcher dispatcher(event);
        dispatcher.dispatch<Core::WindowCloseEvent>(
            [this](Core::WindowCloseEvent&)
            {
                ++m_CloseEventsSeen;
                return m_Veto;
            });
    }

    [[nodiscard]] int closeEventsSeen() const
    {
        return m_CloseEventsSeen;
    }

  private:
    bool m_Veto;
    int m_CloseEventsSeen = 0;
};

/// Layer that calls Window::requestClose() on its first update, as the custom title bar's Close
/// button does, and stops the app itself after `stopAfter` updates so a vetoed close still ends.
class CloseRequestingLayer : public Core::Layer
{
  public:
    explicit CloseRequestingLayer(int stopAfter) : Layer("CloseRequester"), m_StopAfter(stopAfter)
    {}

    void onUpdate(float /*deltaTime*/) override
    {
        ++m_UpdateCount;
        if (m_UpdateCount == 1)
        {
            Core::Application::get().getWindow().requestClose();
        }
        if (m_UpdateCount >= m_StopAfter)
        {
            Core::Application::get().stop();
        }
    }

    [[nodiscard]] int updateCount() const
    {
        return m_UpdateCount;
    }

  private:
    int m_StopAfter;
    int m_UpdateCount = 0;
};

/// Layer that pushes one SDL event of `eventType` (addressed to the app's window) on its first update,
/// as the OS would deliver it, and stops the app itself after `stopAfter` updates so a run whose
/// event does not stop it still ends.
class SdlEventPushingLayer : public Core::Layer
{
  public:
    SdlEventPushingLayer(std::uint32_t eventType, int stopAfter) : Layer("SdlEventPusher"), m_EventType(eventType), m_StopAfter(stopAfter)
    {}

    void onUpdate(float /*deltaTime*/) override
    {
        ++m_UpdateCount;
        if (m_UpdateCount == 1)
        {
            SDL_Event event{};
            event.type = m_EventType;
            if (m_EventType >= SDL_EVENT_WINDOW_FIRST && m_EventType <= SDL_EVENT_WINDOW_LAST)
            {
                event.window.windowID = SDL_GetWindowID(Core::Application::get().getWindow().getHandle());
            }
            m_Pushed = SDL_PushEvent(&event);
        }
        if (m_UpdateCount >= m_StopAfter)
        {
            Core::Application::get().stop();
        }
    }

    [[nodiscard]] int updateCount() const
    {
        return m_UpdateCount;
    }

    [[nodiscard]] bool pushed() const
    {
        return m_Pushed;
    }

  private:
    std::uint32_t m_EventType;
    int m_StopAfter;
    int m_UpdateCount = 0;
    bool m_Pushed = false;
};

/// Layer whose onDetach() logs to g_DetachOrder, then throws, to verify detachAllLayers() still
/// detaches the layers below it (#1124).
class ThrowingOnDetachLayer : public Core::Layer
{
  public:
    explicit ThrowingOnDetachLayer(const std::string& name) : Layer(name)
    {}

    void onDetach() override;
};

/// Static vector to track layer detach order across Application destruction
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> g_DetachOrder;

/// Layer that logs its name to g_DetachOrder when detached
class TrackedLayer : public Core::Layer
{
  public:
    explicit TrackedLayer(const std::string& name) : Layer(name)
    {}

    void onDetach() override
    {
        g_DetachOrder.push_back(getName());
    }
};

void ThrowingOnDetachLayer::onDetach()
{
    g_DetachOrder.push_back(getName());
    throw std::runtime_error("ThrowingOnDetachLayer::onDetach always throws");
}

void ThrowingOnAttachLayer::onDetach()
{
    g_DetachOrder.push_back(getName());
}

} // namespace

// Test-only accessor: lets unit tests observe m_WindowGeometryChangedThisFrame after calling
// signalWindowGeometryChanged(), which is noexcept and has no other observable effect.
// Declared directly in namespace Core (not inside the anonymous namespace above) so the
// `friend struct ApplicationTestAccessor;` declaration in Application.h resolves to this
// exact type. Same pattern as NVMLGPUProbeTestAccessor in test_WindowsNVMLGPUProbe.cpp.
namespace Core
{
struct ApplicationTestAccessor
{
    [[nodiscard]] static bool geometryChangedThisFrame(const Application& app)
    {
        return app.m_WindowGeometryChangedThisFrame;
    }

    [[nodiscard]] static bool closeRequestAccepted(Application& app)
    {
        return app.closeRequestAccepted();
    }
};
} // namespace Core

// =============================================================================
// Construction and Initialization Tests
// =============================================================================

TEST(ApplicationTest, ConstructWithDefaultSpec)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TestApp";

    try
    {
        Core::Application app(spec);
        EXPECT_EQ(&Core::Application::get(), &app);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, ConstructWithCustomSpec)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "CustomApp";
    spec.Width = 800;
    spec.Height = 600;
    spec.VSync = false;

    try
    {
        Core::Application app(spec);

        const auto& window = app.getWindow();
        EXPECT_EQ(window.getWidth(), 800);
        EXPECT_EQ(window.getHeight(), 600);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SingletonInstanceIsAccessible)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "SingletonTest";

    try
    {
        Core::Application app(spec);
        EXPECT_EQ(&Core::Application::get(), &app);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// Layer Management Tests
// =============================================================================

TEST(ApplicationTest, PushLayerCallsOnAttach)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "LayerTest";

    try
    {
        Core::Application app(spec);

        // Push a layer - it will be attached during the push
        app.pushLayer<TestLayer>("TestLayer");

        // Layer should have been attached during pushLayer call
        // (We can't easily verify this without exposing internals,
        // but if it crashes or throws, the test will fail)
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, CloseRequestIsAcceptedUnlessALayerVetoesIt)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "CloseVetoTest";

    try
    {
        Core::Application app(spec);

        // No handler: the close goes ahead.
        EXPECT_TRUE(Core::ApplicationTestAccessor::closeRequestAccepted(app));

        // A layer that only observes the close returns false and does not stop it (#1073).
        auto& observer = app.pushLayer<CloseListenerLayer>("Observer", false);
        EXPECT_TRUE(Core::ApplicationTestAccessor::closeRequestAccepted(app));
        EXPECT_EQ(observer.closeEventsSeen(), 1);

        // A layer that handles it vetoes the close, and the layers below it never see the event.
        auto& vetoer = app.pushLayer<CloseListenerLayer>("Vetoer", true);
        EXPECT_FALSE(Core::ApplicationTestAccessor::closeRequestAccepted(app));
        EXPECT_EQ(vetoer.closeEventsSeen(), 1);
        EXPECT_EQ(observer.closeEventsSeen(), 1);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, RequestCloseRaisesWindowCloseEventAndStopsWhenAccepted)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "RequestCloseAcceptedTest";

    try
    {
        Core::Application app(spec);
        const auto& observer = app.pushLayer<CloseListenerLayer>("Observer", false);
        // The fallback stop is far off: the close request should end the loop long before it.
        const auto& requester = app.pushLayer<CloseRequestingLayer>(100);

        app.run();

        // Window::requestClose() reaches layers as a WindowCloseEvent (#1077), and with no veto the
        // loop ends on the frame after the request instead of running on to the fallback stop.
        EXPECT_EQ(observer.closeEventsSeen(), 1);
        EXPECT_EQ(requester.updateCount(), 1);
        EXPECT_FALSE(app.getWindow().shouldClose());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, RequestCloseIsVetoedByAHandlingLayer)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "RequestCloseVetoedTest";

    try
    {
        Core::Application app(spec);
        const auto& vetoer = app.pushLayer<CloseListenerLayer>("Vetoer", true);
        constexpr int STOP_AFTER = 3;
        const auto& requester = app.pushLayer<CloseRequestingLayer>(STOP_AFTER);

        app.run();

        // The veto keeps the loop running until the layer's own stop, and the request is cleared
        // once raised, so it is not raised again on every later frame.
        EXPECT_EQ(vetoer.closeEventsSeen(), 1);
        EXPECT_EQ(requester.updateCount(), STOP_AFTER);
        EXPECT_FALSE(app.getWindow().shouldClose());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// #1150: SDL's default posts SDL_EVENT_QUIT after a close request on the last window, so one Alt+F4
// raised two WindowCloseEvents and the QUIT would override a veto. Application turns that off.
TEST(ApplicationTest, ClosingTheLastWindowDoesNotAlsoPostQuit)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "QuitOnLastWindowCloseTest";

    try
    {
        const Core::Application app(spec);
        EXPECT_FALSE(SDL_GetHintBoolean(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, true));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// #1150: a close request (Alt+F4, the OS close button) raises exactly one WindowCloseEvent, and a
// veto keeps the app running.
TEST(ApplicationTest, CloseRequestedRaisesOneVetoableWindowCloseEvent)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "CloseRequestedOnceTest";

    try
    {
        Core::Application app(spec);
        const auto& vetoer = app.pushLayer<CloseListenerLayer>("Vetoer", true);
        constexpr int STOP_AFTER = 5;
        const auto& pusher = app.pushLayer<SdlEventPushingLayer>(SDL_EVENT_WINDOW_CLOSE_REQUESTED, STOP_AFTER);

        app.run();

        ASSERT_TRUE(pusher.pushed()) << SDL_GetError();
        EXPECT_EQ(vetoer.closeEventsSeen(), 1);
        EXPECT_EQ(pusher.updateCount(), STOP_AFTER);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// #1150: SIGINT, SIGTERM and OS logout arrive as SDL_EVENT_QUIT. That is not a close request a
// layer may veto: it raises no WindowCloseEvent and always stops the app.
TEST(ApplicationTest, SdlQuitStopsTheAppAndCannotBeVetoed)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "QuitNotVetoableTest";

    try
    {
        Core::Application app(spec);
        const auto& vetoer = app.pushLayer<CloseListenerLayer>("Vetoer", true);
        // The fallback stop is far off: the QUIT should end the loop long before it.
        constexpr int STOP_AFTER = 100;
        const auto& pusher = app.pushLayer<SdlEventPushingLayer>(SDL_EVENT_QUIT, STOP_AFTER);

        app.run();

        ASSERT_TRUE(pusher.pushed()) << SDL_GetError();
        EXPECT_EQ(vetoer.closeEventsSeen(), 0);
        EXPECT_LT(pusher.updateCount(), STOP_AFTER);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, PushMultipleLayers)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "MultiLayerTest";

    try
    {
        Core::Application app(spec);

        app.pushLayer<TestLayer>("Layer1");
        app.pushLayer<TestLayer>("Layer2");
        app.pushLayer<TestLayer>("Layer3");

        // All layers pushed successfully (would crash if not)
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// FramePacing Tests (pure decision logic extracted from Application::run())
// =============================================================================

TEST(FramePacingTest, ShouldBreakEventDrainWhenBudgetExceeded)
{
    EXPECT_TRUE(Core::FramePacing::computeShouldBreakEventDrain(8.0, 8.0));
    EXPECT_TRUE(Core::FramePacing::computeShouldBreakEventDrain(9.0, 8.0));
    EXPECT_FALSE(Core::FramePacing::computeShouldBreakEventDrain(7.9, 8.0));
}

TEST(FramePacingTest, IsInteractingWhenAnyReasonPresent)
{
    EXPECT_TRUE(Core::FramePacing::computeIsInteracting(true, false, 0));
    EXPECT_TRUE(Core::FramePacing::computeIsInteracting(false, true, 0));
    EXPECT_TRUE(Core::FramePacing::computeIsInteracting(false, false, 1));
    EXPECT_FALSE(Core::FramePacing::computeIsInteracting(false, false, 0));
}

TEST(FramePacingTest, FrameDeltaStaysExactAtLargeUptime)
{
    // #1038: at 2^18 s (about three days) of uptime a float clock's spacing is ~31 ms, so a 16 ms
    // frame read as 0 or ~31 ms. The double clock from nanosecond ticks keeps it at 16 ms.
    constexpr std::uint64_t NS_PER_SECOND = 1'000'000'000;
    constexpr std::uint64_t uptimeNs = (std::uint64_t{1} << 18U) * NS_PER_SECOND;
    constexpr std::uint64_t frameNs = 16'000'000;

    const double previous = Core::FramePacing::ticksNsToSeconds(uptimeNs);
    const double current = Core::FramePacing::ticksNsToSeconds(uptimeNs + frameNs);
    EXPECT_NEAR(Core::FramePacing::frameDeltaSeconds(previous, current, 0.1F), 0.016F, 1.0e-6F);

    // The same reading through a float clock, for contrast: the delta is quantised away from 16 ms.
    const auto floatPrevious = static_cast<float>(previous);
    const auto floatCurrent = static_cast<float>(current);
    EXPECT_GT(std::abs((floatCurrent - floatPrevious) - 0.016F), 0.01F);

    // Still capped at the maximum delta.
    EXPECT_FLOAT_EQ(Core::FramePacing::frameDeltaSeconds(previous, previous + 5.0, 0.1F), 0.1F);
}

TEST(FramePacingTest, FrameIntervalIsNotCappedLikeTheAnimationDelta)
{
    // #1152: a 150 ms frame is 150 ms for the FPS readout, though animation still sees at most 100 ms.
    EXPECT_FLOAT_EQ(Core::FramePacing::frameDeltaSeconds(10.0, 10.15, 0.1F), 0.1F);
    EXPECT_NEAR(Core::FramePacing::frameIntervalSeconds(10.0, 10.15), 0.15, 1e-9);
    // A clock read out of order is a zero interval, not a negative one.
    EXPECT_DOUBLE_EQ(Core::FramePacing::frameIntervalSeconds(10.0, 9.0), 0.0);
}

TEST(FramePacingTest, FrameWaitHoldsASteadyRateWhateverTheInput)
{
    // #1037: frames start once per period. The wait is the rest of the period since the last frame
    // started, so a frame that took 5 ms waits about 11.7 ms at 60 FPS.
    constexpr double PERIOD = 1.0 / 60.0;
    EXPECT_NEAR(Core::FramePacing::computeFrameWaitSeconds(0.005, PERIOD), PERIOD - 0.005, 1e-12);
    // A frame that already took the whole period (a 60 Hz vsync swap, or a slow frame) waits nothing.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameWaitSeconds(PERIOD, PERIOD), 0.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameWaitSeconds(0.040, PERIOD), 0.0);
}

TEST(FramePacingTest, ALowerRequestedRateGivesALongerWait)
{
    // #1125: a slowly scrolling chart asks for fewer frames than a NowBar easing to a new sample.
    constexpr double REFRESH = 60.0;
    const double slowPeriod = Core::FramePacing::framePeriodSeconds(REFRESH, 30.0);
    const double fastPeriod = Core::FramePacing::framePeriodSeconds(REFRESH, 60.0);
    EXPECT_GT(slowPeriod, fastPeriod);
    EXPECT_GT(Core::FramePacing::computeFrameWaitSeconds(0.005, slowPeriod), Core::FramePacing::computeFrameWaitSeconds(0.005, fastPeriod));
}

TEST(FramePacingTest, AnimationRateIdlesWhenTheMotionNeedsNoMoreThanTheIdleRate)
{
    constexpr double IDLE = 20.0;
    constexpr double MAX = 60.0;
    // Nothing moved visibly: the idle path.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(0.0, IDLE, MAX), 0.0);
    // #1125: a 300 s chart 1000 px wide scrolls ~3.3 px/s, needing ~6.7 fps at half a pixel per
    // frame -- the idle rate already covers it, so it is not paced at 60 fps any more.
    const double slowChartFps = Core::AnimationRequest::framesPerSecondForMotion(1000.0 / 300.0);
    EXPECT_NEAR(slowChartFps, 6.667, 1e-3);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(slowChartFps, IDLE, MAX), 0.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(IDLE, IDLE, MAX), 0.0);
    // Faster motion is paced at what it needs, up to the cap.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(33.0, IDLE, MAX), 33.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(500.0, IDLE, MAX), MAX);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeAnimationRate(Core::AnimationRequest::FULL_RATE, IDLE, MAX), MAX);
}

TEST(FramePacingTest, HiddenWindowsGetNoPacedFrames)
{
    // #1125: an occluded (or minimized) window is not animated, whatever asked; it takes the hidden
    // idle sleep instead.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(60.0, false, true, 60.0), 0.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(60.0, true, true, 60.0), 0.0);
    EXPECT_EQ(Core::FramePacing::computeIdleSleepMs(true, 50, 200), 200);
    // Visible, the animation rate applies.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(33.0, false, false, 60.0), 33.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(0.0, false, false, 60.0), 0.0);
}

TEST(FramePacingTest, InputDrivenFramesAreCapped)
{
    // #1153: input with nothing animating used to render with no wait at all -- the display rate
    // with vsync, unbounded without. Now it is capped at the full frame rate.
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(0.0, true, false, 60.0), 60.0);
    // Input never slows an animation down (#1037: a steady rate whatever the input).
    EXPECT_DOUBLE_EQ(Core::FramePacing::computeFrameRateCap(33.0, true, false, 60.0), 60.0);
    // With vsync off (an interaction, or vsync disabled) a 1 ms frame at 144 Hz waits out the period.
    const double period = Core::FramePacing::framePeriodSeconds(144.0, 60.0);
    EXPECT_GT(Core::FramePacing::computeFrameWaitSeconds(0.001, period), 0.0);
}

TEST(FramePacingTest, FramePeriodIsAWholeNumberOfRefreshes)
{
    // #1126: a fixed 1/60 s period gave one/two vblank gaps at 75 Hz and two/three at 144 Hz. The
    // period is now a whole number of refreshes, n = max(1, floor(refresh / 60)) -- never slower than
    // the target (#1281 review).
    struct Case
    {
        double refreshHz;
        int vblanks;
    };
    for (const Case c : {Case{.refreshHz = 60.0, .vblanks = 1},
                         Case{.refreshHz = 75.0, .vblanks = 1},
                         Case{.refreshHz = 120.0, .vblanks = 2},
                         Case{.refreshHz = 144.0, .vblanks = 2},
                         Case{.refreshHz = 165.0, .vblanks = 2},
                         Case{.refreshHz = 240.0, .vblanks = 4},
                         Case{.refreshHz = 30.0, .vblanks = 1}})
    {
        SCOPED_TRACE(c.refreshHz);
        EXPECT_EQ(Core::FramePacing::vblanksPerFrame(c.refreshHz, 60.0), c.vblanks);
        const double refreshPeriod = 1.0 / c.refreshHz;
        const double period = Core::FramePacing::framePeriodSeconds(c.refreshHz, 60.0);
        const double multiple = period / refreshPeriod;
        EXPECT_NEAR(multiple, std::round(multiple), 1e-9);
        EXPECT_NEAR(multiple, static_cast<double>(c.vblanks), 1e-9);
    }
    // The display rate itself (a move/resize cap) is one refresh.
    EXPECT_EQ(Core::FramePacing::vblanksPerFrame(144.0, Core::AnimationRequest::FULL_RATE), 1);
    EXPECT_NEAR(Core::FramePacing::framePeriodSeconds(144.0, Core::AnimationRequest::FULL_RATE), 1.0 / 144.0, 1e-12);
}

TEST(FramePacingTest, CadenceIsNeverSlowerThanTheTarget)
{
    // #1281 review: rounding picked 3 refreshes (20 FPS) for a 21 FPS motion request at 60 Hz, and
    // 55 FPS for a 60 FPS target at 165 Hz, breaking the half-pixel-per-frame and request contracts.
    for (const double refreshHz : {59.94, 60.0, 75.0, 120.0, 144.0, 165.0, 240.0})
    {
        for (const double target : {7.0, 20.0, 21.0, 30.0, 45.0, 60.0, 61.0})
        {
            SCOPED_TRACE(::testing::Message() << refreshHz << " Hz, target " << target);
            const double period = Core::FramePacing::framePeriodSeconds(refreshHz, target);
            if (target > refreshHz)
            {
                // Faster than the display: one refresh per frame is the most it can show.
                EXPECT_NEAR(period, 1.0 / refreshHz, 1e-12);
                continue;
            }
            EXPECT_LE(period, (1.0 / target) + 1e-9) << "paced slower than requested";
        }
    }
    EXPECT_EQ(Core::FramePacing::vblanksPerFrame(60.0, 21.0), 2);  // 30 FPS, not 20
    EXPECT_EQ(Core::FramePacing::vblanksPerFrame(165.0, 60.0), 2); // 82.5 FPS, not 55
    EXPECT_EQ(Core::FramePacing::vblanksPerFrame(120.0, 60.0), 2); // exact ratios unaffected
}

TEST(FramePacingTest, VsyncPacedFramesPresentEveryNthRefresh)
{
    // #1281 review: simulate several consecutive frames with a vsync-blocking swap. Each frame starts
    // no sooner than one period after the previous start (and not before its swap returned), renders
    // for a fraction of a refresh, then presents at the next vblank. The gaps between presents must
    // all be exactly n refreshes; a period of n - 0.5 refreshes gave alternating one/two-refresh gaps.
    for (const double refreshHz : {120.0, 144.0, 165.0, 240.0})
    {
        SCOPED_TRACE(refreshHz);
        const int vblanks = Core::FramePacing::vblanksPerFrame(refreshHz, 60.0);
        const double refresh = 1.0 / refreshHz;
        const double period = Core::FramePacing::framePeriodSeconds(refreshHz, 60.0);
        const double phase = 0.37 * refresh; // vblanks at phase + k * refresh
        const double renderTime = 0.25 * refresh;
        const auto nextVblankIndex = [&](double t)
        {
            return static_cast<long long>(std::ceil((t - phase) / refresh));
        };

        double start = 0.0;
        long long previousPresent = -1;
        for (int frame = 0; frame < 24; ++frame)
        {
            const long long present = nextVblankIndex(start + renderTime);
            const double swapReturn = phase + (static_cast<double>(present) * refresh);
            if (frame > 2) // after the first frames settle onto the vblank phase
            {
                EXPECT_EQ(present - previousPresent, vblanks) << "frame " << frame;
            }
            previousPresent = present;
            start = std::max(start + period, swapReturn);
        }
    }
    // One refresh per frame: the swap itself takes the period, so the period is only a cap.
    EXPECT_NEAR(Core::FramePacing::framePeriodSeconds(60.0, 60.0), 1.0 / 60.0, 1e-12);
}

TEST(FramePacingTest, UnknownRefreshRateFallsBack)
{
    EXPECT_DOUBLE_EQ(Core::FramePacing::effectiveRefreshHz(0.0, 60.0), 60.0); // SDL: unspecified
    EXPECT_DOUBLE_EQ(Core::FramePacing::effectiveRefreshHz(-1.0, 60.0), 60.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::effectiveRefreshHz(std::numeric_limits<double>::quiet_NaN(), 60.0), 60.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::effectiveRefreshHz(std::numeric_limits<double>::infinity(), 60.0), 60.0);
    EXPECT_DOUBLE_EQ(Core::FramePacing::effectiveRefreshHz(143.98, 60.0), 143.98);
    // With no refresh rate at all the period is the plain target period.
    EXPECT_NEAR(Core::FramePacing::framePeriodSeconds(0.0, 60.0), 1.0 / 60.0, 1e-12);
}

TEST(AnimationRequestTest, ConsumeReportsTheHighestRequestedRateAndClears)
{
    static_cast<void>(Core::AnimationRequest::consume()); // start clear
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0);
    Core::AnimationRequest::request(12.0);
    Core::AnimationRequest::request(40.0);
    Core::AnimationRequest::request(25.0); // several requests in one frame keep the highest
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 40.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0);
    // Non-positive and NaN rates ask for nothing.
    Core::AnimationRequest::request(0.0);
    Core::AnimationRequest::request(-5.0);
    Core::AnimationRequest::request(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0);
    // The unparameterised request asks for the full rate.
    Core::AnimationRequest::request();
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), Core::AnimationRequest::FULL_RATE);
}

TEST(AnimationRequestTest, MotionRateKeepsMovementUnderHalfAPixelPerFrame)
{
    // #1125: 30 px/s needs 60 frames a second to move at most half a pixel per frame.
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::framesPerSecondForMotion(30.0), 60.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::framesPerSecondForMotion(30.0) * Core::AnimationRequest::MAX_MOTION_PIXELS_PER_FRAME, 30.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::framesPerSecondForMotion(0.0), 0.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::framesPerSecondForMotion(-3.0), 0.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::framesPerSecondForMotion(std::numeric_limits<double>::quiet_NaN()), 0.0);
    static_cast<void>(Core::AnimationRequest::consume());
    Core::AnimationRequest::requestForMotion(5.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 10.0);
}

TEST(FramePacingTest, IsWithinInteractionGrace)
{
    EXPECT_TRUE(Core::FramePacing::isWithinInteractionGrace(1.0, 1.5));
    EXPECT_FALSE(Core::FramePacing::isWithinInteractionGrace(1.5, 1.5));
    EXPECT_FALSE(Core::FramePacing::isWithinInteractionGrace(2.0, 1.5));
}

TEST(FramePacingTest, VsyncTransitionDisablesOnInteractionStartWhenSpecRequestsVsync)
{
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(false, true, true, false), Core::FramePacing::VsyncTransition::Disable);
}

TEST(FramePacingTest, VsyncTransitionNoChangeOnInteractionStartWhenSpecDoesNotRequestVsync)
{
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(false, true, false, false), Core::FramePacing::VsyncTransition::NoChange);
}

TEST(FramePacingTest, VsyncTransitionRestoresOnInteractionEndWhenPreviouslyDisabled)
{
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(true, false, true, true), Core::FramePacing::VsyncTransition::Restore);
}

TEST(FramePacingTest, VsyncTransitionNoChangeOnInteractionEndWhenNotPreviouslyDisabled)
{
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(true, false, true, false), Core::FramePacing::VsyncTransition::NoChange);
}

TEST(FramePacingTest, VsyncTransitionNoChangeWhileInteractionStateUnchanged)
{
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(true, true, true, true), Core::FramePacing::VsyncTransition::NoChange);
    EXPECT_EQ(Core::FramePacing::computeVsyncTransition(false, false, true, false), Core::FramePacing::VsyncTransition::NoChange);
}

TEST(FramePacingTest, SkipRenderThisFrameWhenDrainExceedsBudgetAndNotMinimized)
{
    EXPECT_TRUE(Core::FramePacing::computeSkipRenderThisFrame(16.0, 16.0, false));
    EXPECT_FALSE(Core::FramePacing::computeSkipRenderThisFrame(15.9, 16.0, false));
}

TEST(FramePacingTest, SkipRenderThisFrameNeverSkipsWhileMinimized)
{
    EXPECT_FALSE(Core::FramePacing::computeSkipRenderThisFrame(1000.0, 16.0, true));
}

TEST(FramePacingTest, ShouldSleepWhenIdleOutsideGracePeriod)
{
    EXPECT_TRUE(Core::FramePacing::computeShouldSleepWhenIdle(false, true));
}

TEST(FramePacingTest, ShouldSleepWhenIdleInsideGraceButGeometryUnchanged)
{
    EXPECT_TRUE(Core::FramePacing::computeShouldSleepWhenIdle(true, false));
}

TEST(FramePacingTest, ShouldNotSleepWhenIdleInsideGraceWithGeometryChanged)
{
    EXPECT_FALSE(Core::FramePacing::computeShouldSleepWhenIdle(true, true));
}

TEST(FramePacingTest, IdleWaitIsMeasuredFromTheFrameStart)
{
    // #1276: the idle wait runs to 50 ms after the previous frame started, so an 8 ms frame waits 42
    // ms and the idle rate really is 20 FPS (a fixed 50 ms after the frame gave about 17).
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(false, 50, 200, 0.008), 42);
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(false, 50, 200, 0.0), 50);
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(false, 50, 200, 0.0081), 42); // rounded up, never short
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(false, 50, 200, 0.075), 0);   // a slow frame: no wait
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(true, 50, 200, 0.008), 192);  // hidden: 5 FPS period
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(false, 50, 200, -1.0), 50);   // a clock step back
}

TEST(FramePacingTest, IdleSleepMsUsesMinimizedDurationWhenHidden)
{
    EXPECT_EQ(Core::FramePacing::computeIdleSleepMs(true, 50, 200), 200);
    EXPECT_EQ(Core::FramePacing::computeIdleSleepMs(false, 50, 200), 50);
}

// =============================================================================
// ResizePerfTrace Tests (pure accumulator/logging extracted from Application::run()'s
// optional resize-performance tracing; see Core/ResizePerfTrace.h)
// =============================================================================

TEST(ResizePerfTraceStatsTest, HasSamplesReflectsRecordedActivity)
{
    Core::ResizePerfTraceStats stats;
    EXPECT_FALSE(stats.hasSamples());

    stats.recordEventBatch(1, 0, 1.0, 1.0, false);
    EXPECT_TRUE(stats.hasSamples());
}

TEST(ResizePerfTraceStatsTest, HasSamplesTrueAfterFrameOnly)
{
    Core::ResizePerfTraceStats stats;
    stats.recordFrame(false, 1.0, 1.0, 1.0, 1.0);
    EXPECT_TRUE(stats.hasSamples());
}

TEST(ResizePerfTraceStatsTest, RecordEventBatchAccumulatesAndTracksMax)
{
    Core::ResizePerfTraceStats stats;
    stats.recordEventBatch(4, 1, 2.0, 1.5, false);
    stats.recordEventBatch(10, 3, 5.0, 6.0, true);

    EXPECT_EQ(stats.eventBatches, 2U);
    EXPECT_EQ(stats.drainedEvents, 14U);
    EXPECT_EQ(stats.resizeEvents, 4U);
    EXPECT_EQ(stats.maxEventsPerBatch, 10U);
    EXPECT_DOUBLE_EQ(stats.drainMs, 7.0);
    EXPECT_DOUBLE_EQ(stats.maxDrainMs, 5.0);
    EXPECT_DOUBLE_EQ(stats.maxSinglePollBatchMs, 6.0);
    EXPECT_EQ(stats.p0BudgetCapHits, 1U) << "only the second batch fired p0";
}

TEST(ResizePerfTraceStatsTest, RecordFrameAccumulatesAndTracksMax)
{
    Core::ResizePerfTraceStats stats;
    stats.recordFrame(false, 1.0, 2.0, 0.5, 3.0);
    stats.recordFrame(true, 4.0, 1.0, 2.0, 9.0);

    EXPECT_EQ(stats.frames, 2U);
    EXPECT_EQ(stats.resizeFrames, 1U) << "only the second frame was a resize frame";
    EXPECT_DOUBLE_EQ(stats.updateMs, 5.0);
    EXPECT_DOUBLE_EQ(stats.maxUpdateMs, 4.0);
    EXPECT_DOUBLE_EQ(stats.renderMs, 3.0);
    EXPECT_DOUBLE_EQ(stats.maxRenderMs, 2.0);
    EXPECT_DOUBLE_EQ(stats.postRenderMs, 2.5);
    EXPECT_DOUBLE_EQ(stats.maxPostRenderMs, 2.0);
    EXPECT_DOUBLE_EQ(stats.swapMs, 12.0);
    EXPECT_DOUBLE_EQ(stats.maxSwapMs, 9.0);
    // Frame 1 total: 1.0+2.0+0.5+3.0=6.5; frame 2 total: 4.0+1.0+2.0+9.0=16.0.
    EXPECT_DOUBLE_EQ(stats.totalFrameMs, 22.5);
    EXPECT_DOUBLE_EQ(stats.maxTotalFrameMs, 16.0);
}

TEST(ResizePerfTraceStatsTest, RecordFrameAppendsPerPhaseSamples)
{
    Core::ResizePerfTraceStats stats;
    stats.recordFrame(false, 1.0, 2.0, 0.5, 3.0);
    stats.recordFrame(true, 4.0, 1.0, 2.0, 9.0);

    EXPECT_EQ(stats.updateSamplesMs.size(), 2U);
    EXPECT_EQ(stats.renderSamplesMs.size(), 2U);
    EXPECT_EQ(stats.postRenderSamplesMs.size(), 2U);
    EXPECT_EQ(stats.swapSamplesMs.size(), 2U);
    EXPECT_DOUBLE_EQ(stats.updateSamplesMs[1], 4.0);
    EXPECT_DOUBLE_EQ(stats.swapSamplesMs[0], 3.0);

    ASSERT_EQ(stats.totalFrameSamplesMs.size(), 2U);
    EXPECT_DOUBLE_EQ(stats.totalFrameSamplesMs[0], 6.5);
    EXPECT_DOUBLE_EQ(stats.totalFrameSamplesMs[1], 16.0);
}

TEST(ResizePerfTraceStatsTest, RecordEventBatchAppendsDrainSamples)
{
    Core::ResizePerfTraceStats stats;
    stats.recordEventBatch(4, 1, 2.0, 1.5, false);
    stats.recordEventBatch(10, 3, 5.0, 6.0, true);

    ASSERT_EQ(stats.drainSamplesMs.size(), 2U);
    EXPECT_DOUBLE_EQ(stats.drainSamplesMs[0], 2.0);
    EXPECT_DOUBLE_EQ(stats.drainSamplesMs[1], 5.0);
}

TEST(ResizePerfTraceStatsTest, ResetIntervalCountersClearsCountersButKeepsRollingSamples)
{
    Core::ResizePerfTraceStats stats;
    stats.recordEventBatch(4, 1, 2.0, 1.5, false);
    stats.recordFrame(true, 1.0, 2.0, 0.5, 3.0);

    stats.resetIntervalCounters();

    EXPECT_EQ(stats.eventBatches, 0U);
    EXPECT_EQ(stats.frames, 0U);
    EXPECT_EQ(stats.resizeFrames, 0U);
    EXPECT_DOUBLE_EQ(stats.drainMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.updateMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.maxDrainMs, 0.0);
    EXPECT_DOUBLE_EQ(stats.maxTotalFrameMs, 0.0);
    EXPECT_FALSE(stats.hasSamples()) << "hasSamples() reflects this interval's counters, which were reset";

    // Rolling sample windows survive the reset -- this is the whole point of the split.
    ASSERT_EQ(stats.drainSamplesMs.size(), 1U);
    EXPECT_DOUBLE_EQ(stats.drainSamplesMs[0], 2.0);
    ASSERT_EQ(stats.updateSamplesMs.size(), 1U);
    EXPECT_DOUBLE_EQ(stats.updateSamplesMs[0], 1.0);

    // A full aggregate reset (the interaction-transition path) clears everything, including
    // the rolling windows.
    stats = {};
    EXPECT_TRUE(stats.drainSamplesMs.empty());
    EXPECT_TRUE(stats.updateSamplesMs.empty());
}

TEST(ResizePerfTraceStatsTest, RollingSampleWindowIsCappedAtPercentileWindowSize)
{
    Core::ResizePerfTraceStats stats;
    const auto capacity = Core::ResizePerfTraceStats::PERCENTILE_WINDOW_SIZE;
    for (std::size_t i = 0; i < capacity + 50; ++i)
    {
        stats.recordEventBatch(1, 0, static_cast<double>(i), 0.0, false);
    }

    ASSERT_EQ(stats.drainSamplesMs.size(), capacity);
    // Oldest samples (0..49) should have been dropped; the window should now hold 50..(capacity+49).
    EXPECT_DOUBLE_EQ(stats.drainSamplesMs.front(), 50.0);
    EXPECT_DOUBLE_EQ(stats.drainSamplesMs.back(), static_cast<double>(capacity + 49));
}

// =============================================================================
// computePercentile Tests (nearest-rank percentile backing the p95/p99 figures in
// logResizePerfTraceSummary(); perf-plan #843 phase 0)
// =============================================================================

TEST(ComputePercentileTest, EmptyInputReturnsZero)
{
    EXPECT_DOUBLE_EQ(Core::computePercentile({}, 0.95), 0.0);
}

TEST(ComputePercentileTest, SingleValueReturnsThatValue)
{
    EXPECT_DOUBLE_EQ(Core::computePercentile({42.0}, 0.99), 42.0);
}

TEST(ComputePercentileTest, P100ReturnsMax)
{
    EXPECT_DOUBLE_EQ(Core::computePercentile({5.0, 1.0, 3.0, 2.0, 4.0}, 1.0), 5.0);
}

TEST(ComputePercentileTest, P0ReturnsMin)
{
    EXPECT_DOUBLE_EQ(Core::computePercentile({5.0, 1.0, 3.0, 2.0, 4.0}, 0.0), 1.0);
}

TEST(ComputePercentileTest, NearestRankOnHundredSortedValues)
{
    std::vector<double> samples;
    samples.reserve(100);
    for (int i = 1; i <= 100; ++i)
    {
        samples.push_back(static_cast<double>(i));
    }
    // Nearest-rank on 100 samples indexed 0..99: p95 -> index 94 (value 95), p99 -> index 98 (value 99).
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 0.95), 95.0);
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 0.99), 99.0);
}

TEST(ComputePercentileTest, NearestRankOnTenSortedValues)
{
    // A non-100-sized regression case: 100 samples happens to make floor(p*(n-1)) and the
    // correct nearest-rank formula (ceil(p*n)-1) agree, hiding a bug that only shows up at
    // other sample counts. For n=10, p95 must select rank ceil(0.95*10)=10 -> 1-indexed rank
    // 10 -> 0-indexed 9, i.e. the 10th (last) of 10 sorted values -- NOT index 8, which a
    // floor(p*(n-1)) formula would incorrectly select.
    const std::vector<double> samples = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0};
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 0.95), 10.0);
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 0.99), 10.0);
    // p50 on 10 samples: ceil(5.0)=5 -> 0-indexed rank 4 -> value 5.
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 0.50), 5.0);
}

TEST(ComputePercentileTest, OutOfRangePercentileIsClamped)
{
    const std::vector<double> samples = {1.0, 2.0, 3.0};
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, 2.0), 3.0);
    EXPECT_DOUBLE_EQ(Core::computePercentile(samples, -1.0), 1.0);
}

TEST(ResizePerfTraceStatsTest, LogSummaryIsNoopWhenNoSamples)
{
    const Core::ResizePerfTraceStats stats;
    EXPECT_NO_THROW(Core::logResizePerfTraceSummary(stats, "test-empty"));
}

TEST(ResizePerfTraceStatsTest, LogSummaryLogsWhenSamplesPresent)
{
    Core::ResizePerfTraceStats stats;
    stats.recordEventBatch(2, 1, 1.0, 1.0, false);
    stats.recordFrame(true, 1.0, 1.0, 1.0, 1.0);
    EXPECT_NO_THROW(Core::logResizePerfTraceSummary(stats, "test-populated"));
}

TEST(ResizePerfDurationStatsTest, StrictThresholdCountsRetainRareOutliers)
{
    Core::ResizePerfDurationStats stats;
    for (const double ms : {0.0, 100.0, 100.001, 250.0, 250.001, 2221.132})
    {
        stats.record(ms);
    }
    EXPECT_EQ(stats.count, 6U);
    EXPECT_EQ(stats.over100, 4U);
    EXPECT_EQ(stats.over250, 2U);
    EXPECT_DOUBLE_EQ(stats.maxMs, 2221.132);
}

TEST(ResizePerfLoopTimingTest, AccountsForWaitDrainTransitionAndUnmeasuredOverhead)
{
    const Core::ResizePerfLoopTiming timing{.drainMs = 8.0, .waitMs = 50.0, .vsyncMs = 2000.0, .frameMs = 5.0};
    EXPECT_DOUBLE_EQ(timing.otherMs(2070.0), 7.0);
    EXPECT_DOUBLE_EQ(timing.otherMs(2062.999), 0.0);
}

TEST(ResizePerfTraceStatsTest, FrameTailCountsResetWithIntervalNotRollingWindow)
{
    Core::ResizePerfTraceStats stats;
    stats.recordFrame(true, 2221.132, 0.0, 0.0, 0.0);
    stats.recordFrame(false, 0.0, 0.0, 100.0, 0.0);
    EXPECT_EQ(stats.frameTail.count, 2U);
    EXPECT_EQ(stats.frameTail.over100, 1U);
    EXPECT_EQ(stats.frameTail.over250, 1U);
    stats.resetIntervalCounters();
    EXPECT_EQ(stats.frameTail.count, 0U);
    EXPECT_EQ(stats.frameTail.over250, 0U);
    EXPECT_EQ(stats.totalFrameSamplesMs.size(), 2U);
}

namespace
{
class ResizePerfOperationTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        m_Saved = Core::resizePerfOperations();
        Core::resizePerfOperations() = {};
        Core::resizePerfOperations().enabled = true;
        m_PreviousLogger = spdlog::default_logger();
        auto logger = std::make_shared<spdlog::logger>("resize-perf-test", std::make_shared<spdlog::sinks::ostream_sink_mt>(m_Output));
        logger->set_level(spdlog::level::info);
        spdlog::set_default_logger(std::move(logger));
    }
    void TearDown() override
    {
        Core::resizePerfOperations() = m_Saved;
        spdlog::set_default_logger(m_PreviousLogger);
    }

    std::ostringstream m_Output;

  private:
    Core::ResizePerfOperations m_Saved;
    std::shared_ptr<spdlog::logger> m_PreviousLogger;
};
} // namespace

TEST_F(ResizePerfOperationTest, DisabledCallsExecuteOnceAndPreserveReturnWithoutRecording)
{
    auto& trace = Core::resizePerfOperations();
    trace.enabled = false;
    int calls = 0;
    EXPECT_FALSE(Core::traceResizePerfSDL(Core::ResizePerfOperation::SizeCommit,
                                          10,
                                          20,
                                          [&]
                                          {
                                              ++calls;
                                              return false;
                                          }));
    Core::traceResizePerfVoid(Core::ResizePerfOperation::Viewport, 10, 20, [&] { ++calls; });
    EXPECT_EQ(calls, 2);
    for (const auto& duration : trace.durations)
    {
        EXPECT_EQ(duration.count, 0U);
    }
    EXPECT_TRUE(m_Output.str().empty());
}

TEST_F(ResizePerfOperationTest, FinalCommitFailureIsSeparateFromNormalCommit)
{
    EXPECT_TRUE(Core::traceResizePerfSDL(Core::ResizePerfOperation::SizeCommit, 640, 480, [] { return true; }));
    EXPECT_FALSE(Core::traceResizePerfSDL(
        Core::ResizePerfOperation::FinalSizeCommit, 800, 600, [] { return SDL_SetError("synthetic resize failure"); }));
    const auto& trace = Core::resizePerfOperations();
    const auto normal = static_cast<std::size_t>(Core::ResizePerfOperation::SizeCommit);
    const auto final = static_cast<std::size_t>(Core::ResizePerfOperation::FinalSizeCommit);
    EXPECT_EQ(trace.durations[normal].count, 1U);
    EXPECT_EQ(trace.durations[final].count, 1U);
    EXPECT_EQ(trace.failures[normal], 0U);
    EXPECT_EQ(trace.failures[final], 1U);
    EXPECT_EQ(trace.maxRequests[final], (std::pair{800, 600}));
    EXPECT_TRUE(m_Output.str().contains("op=final-size-commit"));
    EXPECT_TRUE(m_Output.str().contains("requestedA=800 requestedB=600 result=false error='synthetic resize failure'"));
}

TEST_F(ResizePerfOperationTest, FinalizationAndSubmissionRemainDistinct)
{
    int calls = 0;
    Core::traceResizePerfVoid(Core::ResizePerfOperation::ImGuiFinalize, 0, 0, [&] { ++calls; });
    Core::traceResizePerfVoid(Core::ResizePerfOperation::OpenGLSubmit, 640, 480, [&] { ++calls; });
    const auto& trace = Core::resizePerfOperations();
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(trace.durations[static_cast<std::size_t>(Core::ResizePerfOperation::ImGuiFinalize)].count, 1U);
    EXPECT_EQ(trace.durations[static_cast<std::size_t>(Core::ResizePerfOperation::OpenGLSubmit)].count, 1U);
}

TEST_F(ResizePerfOperationTest, FirstFrameHasNoFabricatedGapAndSkippedLoopsRemainInNextGap)
{
    const auto frequency = SDL_GetPerformanceFrequency();
    Core::recordResizePerfFrameEnd(frequency);
    EXPECT_EQ(Core::resizePerfOperations().frameGaps.count, 0U);
    Core::recordResizePerfFrameEnd(frequency * 3);
    EXPECT_EQ(Core::resizePerfOperations().frameGaps.count, 1U);
    EXPECT_EQ(Core::resizePerfOperations().frameGaps.over250, 1U);
    EXPECT_DOUBLE_EQ(Core::resizePerfOperations().frameGaps.maxMs, 2000.0);
}

TEST_F(ResizePerfOperationTest, SlowVoidOperationReportsCounterBoundsWithoutInventingGpuStatus)
{
    const auto frequency = SDL_GetPerformanceFrequency();
    Core::recordResizePerfOperation(Core::ResizePerfOperation::OpenGLSubmit, frequency, frequency * 3, std::nullopt, 640, 480);
    const auto output = m_Output.str();
    EXPECT_TRUE(output.contains("op=opengl-submit beginCounter="));
    EXPECT_TRUE(output.contains("endCounter="));
    EXPECT_TRUE(output.contains("duration=2000.000 ms"));
    EXPECT_TRUE(output.contains("result=not-queried error=''"));
}

TEST(ResizePerfClockTest, SubtractsCountersBeforeFloatingPointConversion)
{
    constexpr std::uint64_t LARGE_EPOCH = std::uint64_t{1} << 60U;
    const double expectedMs = 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    EXPECT_DOUBLE_EQ(Core::resizePerfElapsedMs(LARGE_EPOCH, LARGE_EPOCH + 1), expectedMs);
}

// =============================================================================
// Application Lifecycle Tests
// =============================================================================

TEST(ApplicationTest, StopPreventsRunLoop)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "StopTest";

    try
    {
        Core::Application app(spec);

        // Push layer that stops app after 1 update
        app.pushLayer<StopAfterNLayer>(1);

        // Run should exit cleanly after layer requests stop
        app.run();

        // If we get here, run() exited successfully
        SUCCEED();
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, GetTimeReturnsMonotonicValue)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TimeTest";

    try
    {
        Core::Application app(spec);

        const double time1 = Core::Application::getTime();
        const double time2 = Core::Application::getTime();

        // Time should be monotonic
        EXPECT_GE(time2, time1);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, GetTimeIsConsistent)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TimeConsistencyTest";

    try
    {
        Core::Application app(spec);

        const double time1 = Core::Application::getTime();
        const double time2 = Core::Application::getTime();

        // Within a few microseconds, times should be nearly identical
        EXPECT_NEAR(time1, time2, 0.01F); // 10ms tolerance
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// Window Access Tests
// =============================================================================

TEST(ApplicationTest, GetWindowReturnsValidWindow)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "WindowTest";
    spec.Width = 640;
    spec.Height = 480;

    try
    {
        Core::Application app(spec);

        const auto& window = app.getWindow();
        EXPECT_EQ(window.getWidth(), 640);
        EXPECT_EQ(window.getHeight(), 480);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, IsInteractionRedrawActiveIsFalseBeforeAnyInteraction)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "InteractionRedrawTest";

    try
    {
        Core::Application app(spec);
        EXPECT_FALSE(app.isInteractionRedrawActive());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SignalWindowGeometryChangedSetsFlag)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "GeometryChangedTest";

    try
    {
        Core::Application app(spec);
        // Called by TitleBarLayer when it moves/resizes the window; no public getter, so
        // Core::ApplicationTestAccessor (declared above) observes the private flag directly.
        ASSERT_FALSE(Core::ApplicationTestAccessor::geometryChangedThisFrame(app));
        app.signalWindowGeometryChanged();
        EXPECT_TRUE(Core::ApplicationTestAccessor::geometryChangedThisFrame(app));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// Destructor Tests
// =============================================================================

TEST(ApplicationTest, DestructorDetachesLayers)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    // Clear static tracking vector before test
    g_DetachOrder.clear();

    // Create and destroy application with tracked layers
    {
        Core::ApplicationSpecification spec;
        spec.Name = "DestructorTest";

        try
        {
            Core::Application app(spec);

            // Push layers that track their detachment
            app.pushLayer<TrackedLayer>("Layer1");
            app.pushLayer<TrackedLayer>("Layer2");
            app.pushLayer<TrackedLayer>("Layer3");
        }
        catch (const std::exception& e)
        {
            FAIL() << "Application creation failed after the display probe passed: " << e.what();
        }
    }

    // Verify all layers were detached
    ASSERT_EQ(g_DetachOrder.size(), 3) << "All three layers should have been detached";

    // Layers should be detached in reverse order (LIFO - last pushed, first detached)
    EXPECT_EQ(g_DetachOrder[0], "Layer3");
    EXPECT_EQ(g_DetachOrder[1], "Layer2");
    EXPECT_EQ(g_DetachOrder[2], "Layer1");
}

// Regression test for #779: pushLayer() must pop a half-initialized layer back off the
// stack when onAttach() throws, rather than leaving it there for a later detachAllLayers()/
// destructor pass to mistakenly treat as fully attached.
TEST(ApplicationTest, PushLayerPopsHalfInitializedLayerWhenOnAttachThrows)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    g_DetachOrder.clear();

    Core::ApplicationSpecification spec;
    spec.Name = "PushLayerThrowTest";

    try
    {
        Core::Application app(spec);

        EXPECT_THROW(app.pushLayer<ThrowingOnAttachLayer>("Bad"), std::runtime_error);

        // A layer pushed afterward must attach normally -- the failed push didn't corrupt
        // the stack (e.g. leave a stale/half-initialized entry, or a mismatched size).
        app.pushLayer<TrackedLayer>("Good");

        app.detachAllLayers();

        // "Bad" never finished attaching, so onDetach() must never be called for it -- if the
        // failed layer had been left in the stack, detachAllLayers() would have called
        // onDetach() on it here, since it unconditionally iterates the whole layer stack.
        ASSERT_EQ(g_DetachOrder.size(), 1U);
        EXPECT_EQ(g_DetachOrder[0], "Good");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// #1124: a layer that throws while detaching must not stop the layers below it being detached.
TEST(ApplicationTest, DetachAllLayersContinuesPastAThrowingLayer)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    g_DetachOrder.clear();

    Core::ApplicationSpecification spec;
    spec.Name = "DetachThrowTest";

    try
    {
        Core::Application app(spec);
        app.pushLayer<TrackedLayer>("Bottom");
        app.pushLayer<ThrowingOnDetachLayer>("Thrower");
        app.pushLayer<TrackedLayer>("Top");

        EXPECT_NO_THROW(app.detachAllLayers());

        // Topmost first; the throw in "Thrower" doesn't skip "Bottom".
        EXPECT_EQ(g_DetachOrder, (std::vector<std::string>{"Top", "Thrower", "Bottom"}));
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// Copy/Move Semantics Tests
// =============================================================================

TEST(ApplicationTest, ApplicationIsNotCopyable)
{
    EXPECT_FALSE(std::is_copy_constructible_v<Core::Application>);
    EXPECT_FALSE(std::is_copy_assignable_v<Core::Application>);
}

TEST(ApplicationTest, ApplicationIsNotMovable)
{
    EXPECT_FALSE(std::is_move_constructible_v<Core::Application>);
    EXPECT_FALSE(std::is_move_assignable_v<Core::Application>);
}
// =============================================================================
// Singleton setInstance() Tests
// =============================================================================

TEST(ApplicationTest, SetInstanceWithUniquePtr)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "SetInstanceTest";

    try
    {
        auto app = std::make_unique<Core::Application>(spec);
        auto* appPtr = app.get();

        // Explicitly set instance with unique_ptr ownership
        Core::Application::setInstance(std::move(app));

        // get() should return the same instance
        EXPECT_EQ(&Core::Application::get(), appPtr);

        // Cleanup to avoid leaking singleton state across tests
        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SetInstanceOverridesThreadLocalFallback)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "ThreadLocalReplacementTest";

    try
    {
        // Create app without setting instance - constructor sets thread_local fallback
        auto app = std::make_unique<Core::Application>(spec);
        auto* appPtr = app.get();

        // Before setInstance, get() should work via thread_local fallback
        EXPECT_EQ(&Core::Application::get(), appPtr);

        // Now explicitly set instance with unique_ptr; subsequent get() calls come from s_Instance
        Core::Application::setInstance(std::move(app));

        // get() should still return correct instance (now via s_Instance)
        EXPECT_EQ(&Core::Application::get(), appPtr);

        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, GetReturnsCorrectInstanceAfterSetInstance)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "GetAfterSetTest";

    try
    {
        auto app = std::make_unique<Core::Application>(spec);
        auto* expectedApp = app.get();

        Core::Application::setInstance(std::move(app));

        // Multiple calls to get() should return the same instance
        EXPECT_EQ(&Core::Application::get(), expectedApp);
        EXPECT_EQ(&Core::Application::get(), expectedApp);
        EXPECT_EQ(&Core::Application::get(), expectedApp);

        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SetInstancePreservesWindowState)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "WindowPreservationTest";
    spec.Width = 1024;
    spec.Height = 768;

    try
    {
        auto app = std::make_unique<Core::Application>(spec);
        const auto [width, height] = app->getWindow().getSize();

        Core::Application::setInstance(std::move(app));

        // Window properties should be preserved
        auto& instance = Core::Application::get();
        const auto [newWidth, newHeight] = instance.getWindow().getSize();
        EXPECT_EQ(newWidth, width);
        EXPECT_EQ(newHeight, height);

        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SetInstanceAllowsLayerOperations)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "LayerOperationsTest";

    try
    {
        auto app = std::make_unique<Core::Application>(spec);

        // Push layers before setting instance
        auto& layer1 = app->pushLayer<TestLayer>("BeforeSetting");

        Core::Application::setInstance(std::move(app));

        // Should be able to push layers after setting instance
        auto& layer2 = Core::Application::get().pushLayer<TestLayer>("AfterSetting");

        // Layer 1 should still be in the stack.
        // NOTE: layer1 reference remains valid for as long as the Application instance is alive,
        // because setInstance() transfers ownership without moving the Application object itself.
        EXPECT_TRUE(layer1.attachCalled);

        // The second layer pushed after setInstance() should also be properly attached.
        EXPECT_TRUE(layer2.attachCalled);

        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, SetInstanceMaintainsSingletonSemantics)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "SingletonSemanticsTest";

    try
    {
        auto app = std::make_unique<Core::Application>(spec);
        auto* appPtr = app.get();

        Core::Application::setInstance(std::move(app));

        // Calling get() multiple times should always return the same instance
        Core::Application& ref1 = Core::Application::get();
        Core::Application& ref2 = Core::Application::get();
        Core::Application& ref3 = Core::Application::get();

        EXPECT_EQ(&ref1, &ref2);
        EXPECT_EQ(&ref2, &ref3);
        EXPECT_EQ(&ref1, appPtr);

        Core::Application::setInstance(nullptr);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// Constructor Failure / Singleton Cleanup Tests
// =============================================================================

TEST(ApplicationTest, FailedConstructionClearsSingletonAndAllowsRetry)
{
    // This test verifies the constructor catch-block that clears g_StackApplicationInstance
    // when construction fails. Without that cleanup, a dangling Application::get() reference
    // would corrupt the singleton state for all subsequent tests.
    //
    // Forcing failure: overriding the SDL_HINT_VIDEO_DRIVER hint to a non-existent driver
    // makes SDL_Init reject it immediately, so the constructor throws before Window is
    // created. Using SDL_SetHintWithPriority with SDL_HINT_OVERRIDE avoids any POSIX
    // setenv()/getenv() calls (which are unavailable or deprecated on Windows) and also
    // bypasses SDL3's internal environment snapshot, which is populated once at first use
    // and is not updated by subsequent setenv() calls.

    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    // Save the current SDL video driver hint before we clobber it.
    // SDL_GetHint returns a pointer into SDL's internal store; copy it immediately.
    const char* currentHintRaw = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const std::string savedHint = (currentHintRaw != nullptr) ? currentHintRaw : "";

    // Point SDL at a non-existent driver so SDL_Init(SDL_INIT_VIDEO) fails inside
    // the Application constructor, exercising the singleton-cleanup catch block.
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "nosuchdriver", SDL_HINT_OVERRIDE);

    Core::ApplicationSpecification spec;
    spec.Name = "FailureTest";

    // Construction must throw because SDL_Init will reject the invalid driver.
    EXPECT_THROW({ Core::Application failedApp(spec); }, std::exception);

    // After the failed construction the singleton must be cleared;
    // Application::get() must throw rather than returning a dangling reference.
    // static_cast<void> discards the [[nodiscard]] return inside EXPECT_THROW.
    EXPECT_THROW(static_cast<void>(Core::Application::get()), std::runtime_error);

    // Restore the driver hint so the second construction (and all subsequent tests) works.
    // When no driver was previously configured we must NOT force "offscreen" — that would
    // permanently override the hint for all later tests on a real display (X11/Wayland).
    // Instead, reset the hint entirely so SDL reverts to its own driver-selection logic.
    // When a driver was saved, restore it at OVERRIDE priority so it takes effect even
    // though SDL3's internal env snapshot may already be populated.
    if (savedHint.empty())
    {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
    }
    else
    {
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, savedHint.c_str(), SDL_HINT_OVERRIDE);
    }

    // With the valid driver restored, a second Application construction must succeed
    // and the singleton must point at the new instance — no dangling state from above.
    try
    {
        Core::Application app(spec);
        EXPECT_EQ(&Core::Application::get(), &app);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application re-construction after failure probe failed: " << e.what();
    }
}

TEST(ApplicationTest, SetInstanceTransfersOwnershipCorrectly)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "OwnershipTransferTest";

    try
    {
        {
            auto app = std::make_unique<Core::Application>(spec);
            Core::Application::setInstance(std::move(app));
        }

        // After scope ends, the moved-from unique_ptr 'app' is destroyed. It no longer owns
        // the Application object, so its destruction is a no-op. The Application itself
        // remains alive and is owned by s_Instance until we reset it explicitly below.

        // Instance should still be accessible after scope exit (verify get() works)
        EXPECT_NO_THROW([[maybe_unused]] auto& ref = Core::Application::get());
        Core::Application::setInstance(nullptr);

        // After destruction, verify singleton is cleared by checking that get() throws
        bool exceptionThrown = false;
        std::string exceptionMessage;
        try
        {
            // Discarded rather than bound: get() is called for its throw, and binding the
            // reference left an unused local that [[maybe_unused]] did not satisfy CodeQL about
            // (cpp/unused-local-variable).
            static_cast<void>(Core::Application::get());
        }
        catch (const std::runtime_error& e)
        {
            exceptionThrown = true;
            exceptionMessage = e.what();
        }
        EXPECT_TRUE(exceptionThrown);
        EXPECT_STREQ(exceptionMessage.c_str(), "Application does not exist!");

        // Note: We do NOT create a new Application here because SDL_Quit() was called
        // in the destructor above, and re-initializing SDL within the same test process
        // can cause undefined behavior on some platforms. Each test should have its own
        // SDL lifecycle scope.
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// paths() accessor
// =============================================================================

TEST(ApplicationTest, PathsReturnsValidPathService)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "PathsAccessorTest";

    try
    {
        Core::Application app(spec);
        const Core::PathService& paths = app.paths();

        // Both dirs must be non-empty, absolute paths — the same guarantee PathService
        // tests verify, but exercised through the Application accessor here.
        EXPECT_FALSE(paths.executableDir().empty());
        EXPECT_TRUE(paths.executableDir().is_absolute());
        EXPECT_FALSE(paths.userConfigDir().empty());
        EXPECT_TRUE(paths.userConfigDir().is_absolute());
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// raiseEvent() — event dispatch
// =============================================================================

TEST(ApplicationTest, RaiseEventDispatchesToLayersInReverseOrder)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "RaiseEventOrderTest";

    try
    {
        Core::Application app(spec);

        // Push two non-handling layers. dispatchLog records the exact call order so
        // we can assert that Top (pushed last = highest index) is invoked before
        // Bottom, i.e., the layer stack is iterated in reverse-push order.
        std::vector<std::string> dispatchLog;
        app.pushLayer<EventTrackingLayer>("Bottom", /*handleEvents=*/false, &dispatchLog);
        app.pushLayer<EventTrackingLayer>("Top", /*handleEvents=*/false, &dispatchLog);

        Core::WindowCloseEvent event;
        app.raiseEvent(event);

        // Reverse order means Top receives it first, then Bottom.
        ASSERT_EQ(dispatchLog.size(), 2U);
        EXPECT_EQ(dispatchLog[0], "Top");
        EXPECT_EQ(dispatchLog[1], "Bottom");
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

TEST(ApplicationTest, RaiseEventStopsAfterEventIsHandled)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "RaiseEventHandledTest";

    try
    {
        Core::Application app(spec);

        // Top layer handles the event; bottom layer must NOT receive it.
        auto& bottom = app.pushLayer<EventTrackingLayer>("Bottom", /*handleEvents=*/false);
        auto& top = app.pushLayer<EventTrackingLayer>("Top", /*handleEvents=*/true);

        Core::WindowCloseEvent event;
        app.raiseEvent(event);

        EXPECT_EQ(top.receivedEventNames.size(), 1U);
        EXPECT_TRUE(event.isHandled());
        // Bottom should not have received the event because Top handled it.
        EXPECT_EQ(bottom.receivedEventNames.size(), 0U);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// Regression test for #778: a layer that throws from onEvent() must not crash the whole
// process (raiseEvent() previously had no exception guard, unlike onUpdate/onRender/
// onPostRender, so this would have called std::terminate() before the fix). Dispatch must
// also continue to the remaining layers after the throwing one is caught and logged.
TEST(ApplicationTest, RaiseEventDoesNotCrashWhenLayerThrows)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "RaiseEventThrowingLayerTest";

    try
    {
        Core::Application app(spec);

        // Top throws on every event; Bottom is a normal tracking layer below it in the stack.
        // Reverse-order dispatch means Top is invoked first.
        auto& bottom = app.pushLayer<EventTrackingLayer>("Bottom", /*handleEvents=*/false);
        app.pushLayer<ThrowingLayer>("Top");

        Core::WindowCloseEvent event;
        EXPECT_NO_THROW(app.raiseEvent(event));

        // Bottom must still have received the event: dispatch continues past the layer that
        // threw instead of aborting the whole loop.
        EXPECT_EQ(bottom.receivedEventNames.size(), 1U);
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}

// =============================================================================
// WindowResizedEvent dispatch
// =============================================================================

TEST(ApplicationTest, RaiseWindowResizedEventReachesLayers)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "ResizeEventDispatchTest";

    try
    {
        Core::Application app(spec);

        auto& tracker = app.pushLayer<EventTrackingLayer>("Tracker", /*handleEvents=*/false);

        Core::WindowResizedEvent event(1280, 720);
        app.raiseEvent(event);

        ASSERT_EQ(tracker.receivedEventNames.size(), 1U);
        EXPECT_STREQ(tracker.receivedEventNames[0].c_str(), "WindowResized");
        EXPECT_FALSE(event.isHandled()); // layer did not consume it
    }
    catch (const std::exception& e)
    {
        FAIL() << "Application creation failed after the display probe passed: " << e.what();
    }
}
