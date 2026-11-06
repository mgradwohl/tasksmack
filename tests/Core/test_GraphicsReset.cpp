/// @file test_GraphicsReset.cpp
/// @brief Tests for Core/GraphicsReset.h (#3484): a GPU reset that glGetGraphicsResetStatus() reports
/// leads to exactly one OpenGL context replacement, after the driver says the reset is over or after
/// GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS, and a context that never reports one is never replaced.
/// Before #3484 nothing checked: after a Windows TDR the app kept drawing with the degraded context,
/// and every idle frame cost 15-30 ms until it restarted.

#include "Core/GraphicsReset.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Core
{
namespace
{

TEST(GraphicsResetMonitorTest, NoResetNeverAsksForAContext)
{
    GraphicsResetMonitor monitor;
    for (int frame = 0; frame < 1000; ++frame)
    {
        EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, frame * 0.05), GraphicsResetAction::None);
    }
    EXPECT_EQ(monitor.resets(), 0U);
}

TEST(GraphicsResetMonitorTest, ReportedOnceThenClearRecreatesOnTheNextCheck)
{
    // The ARB_robustness form: the status names the reset once, then reads GL_NO_ERROR.
    GraphicsResetMonitor monitor;
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, 10.0), GraphicsResetAction::None);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_INNOCENT, 10.05), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, 10.10), GraphicsResetAction::Recreate);
    EXPECT_EQ(monitor.resets(), 1U);
    EXPECT_EQ(monitor.lostStatus(), GL_RESET_STATUS_INNOCENT);
    // Exactly one replacement: the new context's checks are clean again.
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, 10.15), GraphicsResetAction::None);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, 10.20), GraphicsResetAction::None);
}

TEST(GraphicsResetMonitorTest, WaitsWhileTheResetIsStillReported)
{
    GraphicsResetMonitor monitor;
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_GUILTY, 0.0), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_GUILTY, 0.5), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_GUILTY, 1.5), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_NO_ERROR, 1.6), GraphicsResetAction::Recreate);
    EXPECT_EQ(monitor.resets(), 1U);
    EXPECT_EQ(monitor.lostStatus(), GL_RESET_STATUS_GUILTY);
}

TEST(GraphicsResetMonitorTest, RecreatesAnywayWhenTheResetNeverClears)
{
    // A driver that keeps reporting the lost context must not leave the app on it.
    GraphicsResetMonitor monitor;
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_UNKNOWN, 100.0), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_UNKNOWN, 100.0 + GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS - 0.01), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_UNKNOWN, 100.0 + GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS), GraphicsResetAction::Recreate);
    // The replacement reporting a reset again is a new reset, waited for again.
    EXPECT_EQ(monitor.observe(GL_RESET_STATUS_UNKNOWN, 103.0), GraphicsResetAction::Wait);
    EXPECT_EQ(monitor.resets(), 2U);
}

TEST(GraphicsResetMonitorTest, EachResetGetsItsOwnReplacement)
{
    GraphicsResetMonitor monitor;
    int recreations = 0;
    double now = 0.0;
    // Three resets over a long run, each reported for a few frames, with clean frames between.
    for (int reset = 0; reset < 3; ++reset)
    {
        for (int frame = 0; frame < 50; ++frame, now += 0.045)
        {
            recreations += monitor.observe(GL_RESET_STATUS_NO_ERROR, now) == GraphicsResetAction::Recreate ? 1 : 0;
        }
        for (int frame = 0; frame < 3; ++frame, now += 0.045)
        {
            recreations += monitor.observe(GL_RESET_STATUS_INNOCENT, now) == GraphicsResetAction::Recreate ? 1 : 0;
        }
    }
    recreations += monitor.observe(GL_RESET_STATUS_NO_ERROR, now) == GraphicsResetAction::Recreate ? 1 : 0;
    EXPECT_EQ(recreations, 3);
    EXPECT_EQ(monitor.resets(), 3U);
}

TEST(GraphicsResetTest, StatusNames)
{
    EXPECT_EQ(graphicsResetStatusName(GL_RESET_STATUS_NO_ERROR), "none");
    EXPECT_EQ(graphicsResetStatusName(GL_RESET_STATUS_GUILTY), "guilty");
    EXPECT_EQ(graphicsResetStatusName(GL_RESET_STATUS_INNOCENT), "innocent");
    EXPECT_EQ(graphicsResetStatusName(GL_RESET_STATUS_UNKNOWN), "unknown");
    EXPECT_EQ(graphicsResetStatusName(0x1234U), "unknown");
}

TEST(GraphicsResetTest, GenerationAdvances)
{
    const std::uint64_t before = graphicsContextGeneration();
    advanceGraphicsContextGeneration();
    EXPECT_EQ(graphicsContextGeneration(), before + 1U);
}

} // namespace
} // namespace Core
