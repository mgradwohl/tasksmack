/// @file test_GraphicsReset.cpp
/// @brief Tests for Core/GraphicsReset.h (#3484): a GPU reset that glGetGraphicsResetStatus() reports
/// leads to exactly one OpenGL context replacement, after the driver says the reset is over or after
/// GRAPHICS_RESET_SETTLE_TIMEOUT_SECONDS, and a context that never reports one is never replaced; and
/// GraphicsContextRecovery restores the layers only after a replacement succeeds, retries a failed one
/// after each delay, and gives up after the last.
/// Before #3484 nothing checked: after a Windows TDR the app kept drawing with the degraded context,
/// and every idle frame cost 15-30 ms until it restarted.

#include "Core/GraphicsReset.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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

// GraphicsContextRecovery with fakes: each step appends its name, and recreateContext() answers from
// a script of results (true = the new context was made).
struct RecoveryLog
{
    std::vector<std::string> calls;
    std::vector<bool> recreateResults;
    std::size_t recreateCalls = 0;
};

[[nodiscard]] GraphicsContextRecoverySteps fakeSteps(RecoveryLog& log)
{
    return GraphicsContextRecoverySteps{
        .loseLayers = [&log] { log.calls.emplace_back("lose"); },
        .recreateContext =
            [&log]
        {
            log.calls.emplace_back("recreate");
            const bool made = log.recreateCalls < log.recreateResults.size() && log.recreateResults[log.recreateCalls];
            ++log.recreateCalls;
            return made;
        },
        .advanceGeneration = [&log] { log.calls.emplace_back("advance"); },
        .restoreLayers = [&log] { log.calls.emplace_back("restore"); },
    };
}

TEST(GraphicsContextRecoveryTest, ASuccessfulReplacementRestoresTheLayersAfterTheNewContext)
{
    RecoveryLog log;
    log.recreateResults = {true};
    GraphicsContextRecovery recovery;
    EXPECT_EQ(recovery.replace(1.0, fakeSteps(log)), GraphicsRecoveryState::Rendering);
    EXPECT_EQ(log.calls, (std::vector<std::string>{"lose", "recreate", "advance", "restore"}));
    EXPECT_EQ(recovery.attempts(), 1U);
}

TEST(GraphicsContextRecoveryTest, AFailedReplacementNeverRestoresTheLayers)
{
    // The renderer was shut down by the lose step: restoring the layers, or advancing the generation,
    // with no new context behind them would make GL calls on nothing.
    RecoveryLog log;
    log.recreateResults = {false};
    GraphicsContextRecovery recovery;
    EXPECT_EQ(recovery.replace(1.0, fakeSteps(log)), GraphicsRecoveryState::Suspended);
    EXPECT_EQ(log.calls, (std::vector<std::string>{"lose", "recreate"}));
    EXPECT_EQ(recovery.state(), GraphicsRecoveryState::Suspended);
}

TEST(GraphicsContextRecoveryTest, RetriesAfterTheDelayAndRestoresOnceItWorks)
{
    RecoveryLog log;
    log.recreateResults = {false, false, true};
    GraphicsContextRecovery recovery;
    const auto steps = fakeSteps(log);
    const auto& delays = GraphicsContextRecovery::GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS;
    ASSERT_EQ(recovery.replace(10.0, steps), GraphicsRecoveryState::Suspended);

    // Not before the first delay has passed: no attempt at all.
    EXPECT_EQ(recovery.retryIfDue(10.0 + delays[0] - 0.01, steps), GraphicsRecoveryState::Suspended);
    EXPECT_EQ(log.recreateCalls, 1U);
    // The second attempt fails too, and waits the second delay.
    const double second = 10.0 + delays[0];
    EXPECT_EQ(recovery.retryIfDue(second, steps), GraphicsRecoveryState::Suspended);
    EXPECT_EQ(recovery.retryIfDue(second + delays[1] - 0.01, steps), GraphicsRecoveryState::Suspended);
    EXPECT_EQ(log.recreateCalls, 2U);
    // The third works: only now the generation advances and the layers are restored, once each.
    EXPECT_EQ(recovery.retryIfDue(second + delays[1], steps), GraphicsRecoveryState::Rendering);
    EXPECT_EQ(log.calls, (std::vector<std::string>{"lose", "recreate", "recreate", "recreate", "advance", "restore"}));
    EXPECT_EQ(recovery.attempts(), 3U);
    // Rendering again: retryIfDue() does nothing.
    EXPECT_EQ(recovery.retryIfDue(1000.0, steps), GraphicsRecoveryState::Rendering);
    EXPECT_EQ(log.recreateCalls, 3U);
}

TEST(GraphicsContextRecoveryTest, GivesUpAfterTheLastRetryWithoutEverRestoring)
{
    RecoveryLog log; // every attempt fails
    GraphicsContextRecovery recovery;
    const auto steps = fakeSteps(log);
    double now = 0.0;
    GraphicsRecoveryState state = recovery.replace(now, steps);
    for (const double delay : GraphicsContextRecovery::GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS)
    {
        ASSERT_EQ(state, GraphicsRecoveryState::Suspended);
        now += delay;
        state = recovery.retryIfDue(now, steps);
    }
    EXPECT_EQ(state, GraphicsRecoveryState::GaveUp);
    EXPECT_EQ(recovery.attempts(), GraphicsContextRecovery::GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS.size() + 1U);
    EXPECT_EQ(std::ranges::count(log.calls, std::string("restore")), 0);
    EXPECT_EQ(std::ranges::count(log.calls, std::string("advance")), 0);
    EXPECT_EQ(std::ranges::count(log.calls, std::string("lose")), 1);
    // Given up: no more attempts, however long it waits.
    EXPECT_EQ(recovery.retryIfDue(now + 1000.0, steps), GraphicsRecoveryState::GaveUp);
    EXPECT_EQ(log.recreateCalls, GraphicsContextRecovery::GRAPHICS_RECOVERY_RETRY_DELAYS_SECONDS.size() + 1U);
}

TEST(GraphicsContextRecoveryTest, ANewResetAfterRecoveryStartsAFreshCount)
{
    RecoveryLog log;
    log.recreateResults = {false, true, false, true};
    GraphicsContextRecovery recovery;
    const auto steps = fakeSteps(log);
    ASSERT_EQ(recovery.replace(0.0, steps), GraphicsRecoveryState::Suspended);
    ASSERT_EQ(recovery.retryIfDue(100.0, steps), GraphicsRecoveryState::Rendering);
    ASSERT_EQ(recovery.replace(200.0, steps), GraphicsRecoveryState::Suspended);
    EXPECT_EQ(recovery.attempts(), 1U);
    EXPECT_EQ(recovery.retryIfDue(300.0, steps), GraphicsRecoveryState::Rendering);
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
