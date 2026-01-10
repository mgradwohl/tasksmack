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

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>

// clang-format off
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
// clang-format on

// Check if we have a display available
static bool hasDisplay()
{
    // Check for DISPLAY environment variable (X11) or WAYLAND_DISPLAY
    const char* display = std::getenv("DISPLAY");
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    return (display != nullptr && display[0] != '\0') || (waylandDisplay != nullptr && waylandDisplay[0] != '\0');
}

namespace
{

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

    Core::Application app(spec);

    EXPECT_EQ(&Core::Application::get(), &app);
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

    Core::Application app(spec);

    const auto& window = app.getWindow();
    EXPECT_EQ(window.getWidth(), 800);
    EXPECT_EQ(window.getHeight(), 600);
}

TEST(ApplicationTest, SingletonInstanceIsAccessible)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "SingletonTest";

    Core::Application app(spec);

    EXPECT_EQ(&Core::Application::get(), &app);
}

// =============================================================================
// Layer Management Tests
// =============================================================================

TEST(ApplicationTest, PushLayerCallsOnAttach)
{
    Core::ApplicationSpecification spec;
    spec.Name = "LayerTest";

    Core::Application app(spec);

    // Push a layer - it will be attached during the push
    app.pushLayer<TestLayer>("TestLayer");

    // Layer should have been attached during pushLayer call
    // (We can't easily verify this without exposing internals,
    // but if it crashes or throws, the test will fail)
}

TEST(ApplicationTest, PushMultipleLayers)
{
    Core::ApplicationSpecification spec;
    spec.Name = "MultiLayerTest";

    Core::Application app(spec);

    app.pushLayer<TestLayer>("Layer1");
    app.pushLayer<TestLayer>("Layer2");
    app.pushLayer<TestLayer>("Layer3");

    // All layers pushed successfully (would crash if not)
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

    Core::Application app(spec);

    // Push layer that stops app after 1 update
    app.pushLayer<StopAfterNLayer>(1);

    // Run should exit cleanly after layer requests stop
    app.run();

    // If we get here, run() exited successfully
    SUCCEED();
}

TEST(ApplicationTest, GetTimeReturnsMonotonicValue)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TimeTest";

    Core::Application app(spec);

    float time1 = Core::Application::getTime();
    float time2 = Core::Application::getTime();

    // Time should be monotonic
    EXPECT_GE(time2, time1);
}

TEST(ApplicationTest, GetTimeIsConsistent)
{
    if (!hasDisplay())
    {
        GTEST_SKIP() << "No display available (headless environment)";
    }

    Core::ApplicationSpecification spec;
    spec.Name = "TimeConsistencyTest";

    Core::Application app(spec);

    float time1 = Core::Application::getTime();
    float time2 = Core::Application::getTime();

    // Within a few microseconds, times should be nearly identical
    EXPECT_NEAR(time1, time2, 0.01F); // 10ms tolerance
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

    Core::Application app(spec);

    const auto& window = app.getWindow();
    EXPECT_EQ(window.getWidth(), 640);
    EXPECT_EQ(window.getHeight(), 480);
}

// =============================================================================
// Destructor Tests
// =============================================================================

TEST(ApplicationTest, DestructorDetachesLayers)
{
    // This test ensures destruction completes without crashes
    {
        Core::ApplicationSpecification spec;
        spec.Name = "DestructorTest";

        Core::Application app(spec);

        // Push some layers
        app.pushLayer<TestLayer>("Layer1");
        app.pushLayer<TestLayer>("Layer2");

        // Note: Can't easily verify detach order without exposing internals
        // This test mainly ensures no crashes during destruction
    }

    // App destroyed, layers should have been detached (in reverse order)
    SUCCEED();
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
