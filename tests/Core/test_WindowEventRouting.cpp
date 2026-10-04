/// @file test_WindowEventRouting.cpp
/// @brief Tests for how Application::run() routes SDL window events (Core/WindowEventRouting.h):
/// SDL_EVENT_QUIT is a non-vetoable termination rather than a second close request (#1150), and
/// SDL_EVENT_WINDOW_EXPOSED is a redraw rather than a resize interaction (#1154), and display
/// changes re-read the refresh rate (#1126).

#include "Core/WindowEventRouting.h"

#include <SDL3/SDL_events.h>
#include <gtest/gtest.h>

namespace Core::WindowEventRouting
{
namespace
{

TEST(WindowEventRoutingTest, CloseRequestIsTheOnlyVetoableClose)
{
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_CLOSE_REQUESTED), Action::VetoableClose);
}

TEST(WindowEventRoutingTest, QuitIsNotACloseRequest)
{
    // SIGINT/SIGTERM and OS logout arrive as SDL_EVENT_QUIT; it must not be raised as a (vetoable)
    // WindowCloseEvent, which also made one Alt+F4 raise two of them.
    EXPECT_EQ(classify(SDL_EVENT_QUIT), Action::Quit);
    EXPECT_NE(classify(SDL_EVENT_QUIT), Action::VetoableClose);
}

TEST(WindowEventRoutingTest, ResizeAndMoveEventsAreClassified)
{
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED), Action::PixelSizeChanged);
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_RESIZED), Action::Resized);
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_MOVED), Action::Moved);
}

TEST(WindowEventRoutingTest, ExposedIsARedrawNotAResize)
{
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_EXPOSED), Action::Exposed);
    EXPECT_NE(classify(SDL_EVENT_WINDOW_EXPOSED), Action::Resized);
}

TEST(WindowEventRoutingTest, DisplayChangesRereadTheRefreshRate)
{
    // Moving to a 144 Hz monitor, or a mode change on this one, changes the rate frames pace to (#1126).
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_DISPLAY_CHANGED), Action::DisplayChanged);
    EXPECT_EQ(classify(SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED), Action::DisplayChanged);
}

TEST(WindowEventRoutingTest, OtherEventsNeedNoRouting)
{
    EXPECT_EQ(classify(SDL_EVENT_KEY_DOWN), Action::None);
    EXPECT_EQ(classify(SDL_EVENT_MOUSE_MOTION), Action::None);
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_FOCUS_GAINED), Action::None);
    EXPECT_EQ(classify(SDL_EVENT_WINDOW_MAXIMIZED), Action::None);
}

TEST(WindowEventRoutingTest, ExposeAtTheSameSizeIsNotAResize)
{
    // Uncovering or repainting the window: no WindowResizedEvent, no vsync toggle, no throttle.
    EXPECT_FALSE(exposeChangesSize({1280, 720}, {1280, 720}));
}

TEST(WindowEventRoutingTest, ExposeRevealingANewSizeIsAResize)
{
    // The Windows border-drag fallback: an expose that surfaces before PIXEL_SIZE_CHANGED.
    EXPECT_TRUE(exposeChangesSize({1280, 720}, {1300, 720}));
    EXPECT_TRUE(exposeChangesSize({1280, 720}, {1280, 700}));
}

TEST(WindowEventRoutingTest, ExposeWithAnEmptyFramebufferIsNotAResize)
{
    // A minimized window can report a zero-sized framebuffer.
    EXPECT_FALSE(exposeChangesSize({1280, 720}, {0, 0}));
    EXPECT_FALSE(exposeChangesSize({1280, 720}, {1280, 0}));
}

TEST(WindowEventRoutingTest, AZeroSizedFramebufferIsNotARealSize)
{
    // #1253 review: a minimised window's 0x0 PIXEL_SIZE_CHANGED must not count as a resize
    // interaction, or minimising starts the interaction frame pacing.
    EXPECT_FALSE(isRealPixelSize({0, 0}));
    EXPECT_FALSE(isRealPixelSize({1280, 0}));
    EXPECT_FALSE(isRealPixelSize({0, 720}));
    EXPECT_FALSE(isRealPixelSize({-1, 720}));
    EXPECT_TRUE(isRealPixelSize({1280, 720}));
}

} // namespace
} // namespace Core::WindowEventRouting
