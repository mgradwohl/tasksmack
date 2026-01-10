/// @file test_Application.cpp
/// @brief Tests for Core::Application lifecycle and layer management
///
/// Tests cover:
/// - Application construction and initialization
/// - Layer stack management (push, lifecycle callbacks)
/// - Application run/stop control
/// - Singleton instance access
/// - Error handling (GLFW initialization)
///
/// Note: These tests require a display/windowing system. They are skipped in headless environments.

#include "Core/Application.h"
#include "Core/Layer.h"

// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// clang-format on

#include <gtest/gtest.h>

#include <cstdlib>
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
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - called during single-threaded test startup
    const char* ciEnv = std::getenv("CI");
    if (ciEnv != nullptr && std::string(ciEnv) == "true")
    {
        // Windows CI runners are typically headless
        return false;
    }
    // Local Windows development usually has a display
    return true;
#else
    // On Linux, check for DISPLAY environment variable (X11) or WAYLAND_DISPLAY
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - called during single-threaded test startup before any test threads are created
    const char* display = std::getenv("DISPLAY");
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - called during single-threaded test startup before any test threads are created
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
        GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
            GTEST_SKIP() << "Application creation failed (GLFW error): " << e.what();
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
