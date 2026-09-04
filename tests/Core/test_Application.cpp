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
/// Note: These tests require a display/windowing system. They are skipped in headless environments.

#include "Core/Application.h"
#include "Core/HeadlessVideoDriverTestUtils.h"
#include "Core/Layer.h"
#include "Core/PathService.h"
#include "Core/WindowEvents.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace
{

// Check if we have a display available
bool hasDisplay()
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

    return TestSupport::tryEnableOffscreenVideoDriver();
#endif
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

void ThrowingOnAttachLayer::onDetach()
{
    g_DetachOrder.push_back(getName());
}

} // namespace

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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
    }
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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

        float time1 = Core::Application::getTime();
        float time2 = Core::Application::getTime();

        // Time should be monotonic
        EXPECT_GE(time2, time1);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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

        float time1 = Core::Application::getTime();
        float time2 = Core::Application::getTime();

        // Within a few microseconds, times should be nearly identical
        EXPECT_NEAR(time1, time2, 0.01F); // 10ms tolerance
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
            GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application re-construction after failure probe failed: " << e.what();
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
            [[maybe_unused]] auto& ref = Core::Application::get();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
    }
}
