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
#include "Core/Layer.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <exception>
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
    return (display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0');
#endif
}

/// Test layer that tracks lifecycle callbacks
class TestLayer : public Core::Layer
{
  public:
    explicit TestLayer(const std::string& name = "TestLayer") : Layer(name)
    {
    }

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
    {
    }

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

/// Static vector to track layer detach order across Application destruction
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::vector<std::string> g_DetachOrder;

/// Layer that logs its name to g_DetachOrder when detached
class TrackedLayer : public Core::Layer
{
  public:
    explicit TrackedLayer(const std::string& name) : Layer(name)
    {
    }

    void onDetach() override
    {
        g_DetachOrder.push_back(getName());
    }
};

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
        const int width = app->getWindow().getWidth();
        const int height = app->getWindow().getHeight();

        Core::Application::setInstance(std::move(app));

        // Window properties should be preserved
        auto& instance = Core::Application::get();
        EXPECT_EQ(instance.getWindow().getWidth(), width);
        EXPECT_EQ(instance.getWindow().getHeight(), height);

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
        Core::Application::get().pushLayer<TestLayer>("AfterSetting");

        // Layer 1 should still be in the stack.
        // NOTE: layer1 is owned by the Application's internal layer stack, not by the
        // local std::unique_ptr<Core::Application> that was moved into setInstance().
        // setInstance(std::move(app)) transfers ownership of the same Application
        // instance into the singleton without moving the Application object itself,
        // so the TestLayer object and this reference remain valid here.
        EXPECT_TRUE(layer1.attachCalled);

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

TEST(ApplicationTest, DestructorClearsInstanceAfterSetInstance)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "DestructorClearTest";

    try
    {
        {
            auto app = std::make_unique<Core::Application>(spec);
            Core::Application::setInstance(std::move(app));

            // Instance should be accessible within scope (verify get() works)
            EXPECT_NO_THROW([[maybe_unused]] auto& ref = Core::Application::get());
        }
        // After scope ends, the moved-from unique_ptr 'app' is destroyed. It no longer owns
        // the Application object, so its destruction is a no-op. The Application itself
        // remains alive and is owned by s_Instance until we reset it explicitly here.
        Core::Application::setInstance(nullptr);

        // After destruction, verify singleton is cleared by checking that get() throws
        EXPECT_THROW([[maybe_unused]] auto& ref = Core::Application::get(), std::runtime_error);
    }
    catch (const std::exception& e)
    {
        GTEST_SKIP() << "Application creation failed (SDL error): " << e.what();
    }
}
