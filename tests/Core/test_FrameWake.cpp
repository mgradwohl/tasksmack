/// @file test_FrameWake.cpp
/// @brief A static tab redraws only when something changes (#3156 stage 2): the wait a frame that asked
/// for no motion takes (the slow floor, a grace, motion the floor can't meet, a frame deadline), the
/// frame deadline content asks for, the publication hook a sampler calls, and the coalescer that keeps
/// at most one wake event pending.

#include "Core/AnimationRequest.h"
#include "Core/FramePacing.h"
#include "Core/FrameWake.h"
#include "Core/WindowConstants.h"
#include "Domain/PublicationWake.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <vector>

namespace
{

using Core::FramePacing::computeIdlePeriodMs;
using Core::FramePacing::IdleFramePeriods;

constexpr IdleFramePeriods PERIODS{.idleMs = 50, .hiddenMs = 200, .staticFloorMs = 500};
constexpr double INF = std::numeric_limits<double>::infinity();

// ---- The wait decision ----

TEST(StaticFrameWaitTest, AFrameThatAskedForNothingWaitsForTheFloor)
{
    EXPECT_EQ(computeIdlePeriodMs({}, PERIODS), 500);
    EXPECT_EQ(Core::FramePacing::computeIdleWaitMs(computeIdlePeriodMs({}, PERIODS), 0.010), 490);
}

TEST(StaticFrameWaitTest, MotionTheFloorCantMeetKeepsTheIdleRate)
{
    // A slow history chart scrolling at 6.7 fps, a NowBar's ease, the full rate: the chart's cadence
    // stays the 20 fps idle rate it had (#1125).
    for (const double fps : {2.01, 6.7, 20.0, Core::AnimationRequest::FULL_RATE})
    {
        EXPECT_EQ(computeIdlePeriodMs({.requestedFps = fps}, PERIODS), 50) << fps;
    }
    // The floor already gives 2 fps: the tail of an ease that needs no more asks for nothing more.
    for (const double fps : {0.001, 1.0, 2.0})
    {
        EXPECT_EQ(computeIdlePeriodMs({.requestedFps = fps}, PERIODS), 500) << fps;
    }
}

TEST(StaticFrameWaitTest, AGraceRunningKeepsTheIdleRate)
{
    // An input grace (#843) or a move/resize grace: ImGui finishes reacting at the idle rate at least.
    EXPECT_EQ(computeIdlePeriodMs({.graceActive = true}, PERIODS), 50);
}

TEST(StaticFrameWaitTest, AHiddenWindowThatStillDrawsKeepsTheHiddenRate)
{
    EXPECT_EQ(computeIdlePeriodMs({.isHidden = true}, PERIODS), 200);
    EXPECT_EQ(computeIdlePeriodMs({.requestedFps = 60.0, .isHidden = true, .graceActive = true}, PERIODS), 200);
}

TEST(StaticFrameWaitTest, AFrameDeadlineEndsTheWaitButNeverBeforeTheIdleRate)
{
    EXPECT_EQ(computeIdlePeriodMs({.frameWithinSeconds = 0.2}, PERIODS), 200);
    EXPECT_EQ(computeIdlePeriodMs({.frameWithinSeconds = 0.1234}, PERIODS), 124); // rounded up, never short
    // Sooner than the idle rate, or already due: the idle rate, so nothing can spin the loop.
    for (const double seconds : {0.01, 0.0, -1.0})
    {
        EXPECT_EQ(computeIdlePeriodMs({.frameWithinSeconds = seconds}, PERIODS), 50) << seconds;
    }
    // Later than the floor, none, or nonsense: the floor.
    for (const double seconds : {0.6, INF, std::numeric_limits<double>::quiet_NaN()})
    {
        EXPECT_EQ(computeIdlePeriodMs({.frameWithinSeconds = seconds}, PERIODS), 500) << seconds;
    }
    // A deadline never lengthens a shorter period.
    EXPECT_EQ(computeIdlePeriodMs({.requestedFps = 30.0, .frameWithinSeconds = 0.3}, PERIODS), 50);
    EXPECT_EQ(computeIdlePeriodMs({.frameWithinSeconds = 0.1, .isHidden = true}, PERIODS), 100);
}

TEST(StaticFrameWaitTest, TheFloorIsOneToTwoFramesASecond)
{
    // ImGui's timers still advance, and nothing waits longer than a second for a frame.
    EXPECT_GE(Core::STATIC_FRAME_FLOOR_INTERVAL, std::chrono::milliseconds(500));
    EXPECT_LE(Core::STATIC_FRAME_FLOOR_INTERVAL, std::chrono::milliseconds(1000));
}

// ---- The frame deadline ----

TEST(FrameWithinRequestTest, KeepsTheSoonestAndClears)
{
    static_cast<void>(Core::AnimationRequest::consumeFrameWithin()); // start clear
    EXPECT_EQ(Core::AnimationRequest::consumeFrameWithin(), INF);
    Core::AnimationRequest::requestFrameWithin(0.8);
    Core::AnimationRequest::requestFrameWithin(0.3);
    Core::AnimationRequest::requestFrameWithin(0.5);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consumeFrameWithin(), 0.3);
    EXPECT_EQ(Core::AnimationRequest::consumeFrameWithin(), INF);
}

TEST(FrameWithinRequestTest, ADueDeadlineIsNowAndNaNOrInfinityAskForNothing)
{
    static_cast<void>(Core::AnimationRequest::consumeFrameWithin());
    Core::AnimationRequest::requestFrameWithin(std::numeric_limits<double>::quiet_NaN());
    Core::AnimationRequest::requestFrameWithin(INF);
    EXPECT_EQ(Core::AnimationRequest::consumeFrameWithin(), INF);
    Core::AnimationRequest::requestFrameWithin(-2.0);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consumeFrameWithin(), 0.0);
}

TEST(FrameWithinRequestTest, IsSeparateFromTheMotionRate)
{
    static_cast<void>(Core::AnimationRequest::consume());
    static_cast<void>(Core::AnimationRequest::consumeFrameWithin());
    Core::AnimationRequest::requestFrameWithin(0.25);
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consume(), 0.0) << "a deadline is not motion";
    EXPECT_DOUBLE_EQ(Core::AnimationRequest::consumeFrameWithin(), 0.25);
}

// ---- The publication hook ----

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables) - the hook takes a plain function
std::atomic<int> g_HookCalls{0};

void countHookCall() noexcept
{
    g_HookCalls.fetch_add(1);
}

TEST(PublicationWakeTest, NotifyCallsTheInstalledHandlerAndNothingWithoutOne)
{
    g_HookCalls = 0;
    const Domain::PublicationWake::Handler before = Domain::PublicationWake::setHandler(&countHookCall);
    EXPECT_EQ(before, nullptr) << "no loop runs in this test binary's process";
    Domain::PublicationWake::notify();
    Domain::PublicationWake::notify();
    EXPECT_EQ(g_HookCalls.load(), 2);
    EXPECT_EQ(Domain::PublicationWake::setHandler(nullptr), &countHookCall);
    Domain::PublicationWake::notify(); // no handler: nothing happens
    EXPECT_EQ(g_HookCalls.load(), 2);
}

// ---- The coalescer ----

TEST(WakeCoalescerTest, AtMostOneWakeIsPending)
{
    Core::WakeCoalescer coalescer;
    int pushes = 0;
    const auto push = [&pushes]
    {
        ++pushes;
        return true;
    };
    EXPECT_FALSE(coalescer.pending());
    EXPECT_TRUE(coalescer.notify(push));
    EXPECT_TRUE(coalescer.pending());
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(coalescer.notify(push)) << "coalesced into the pending wake";
    }
    EXPECT_EQ(pushes, 1);
    coalescer.taken();
    EXPECT_FALSE(coalescer.pending());
    EXPECT_TRUE(coalescer.notify(push)) << "a publication after the drain took the wake pushes a new one";
    EXPECT_EQ(pushes, 2);
}

TEST(WakeCoalescerTest, AFailedPushLeavesNoWakePending)
{
    Core::WakeCoalescer coalescer;
    EXPECT_FALSE(coalescer.notify([] { return false; })); // a full queue
    EXPECT_FALSE(coalescer.pending());
    EXPECT_TRUE(coalescer.notify([] { return true; })) << "the next publication tries again";
}

TEST(WakeCoalescerTest, PausedItPushesNothing)
{
    Core::WakeCoalescer coalescer;
    int pushes = 0;
    const auto push = [&pushes]
    {
        ++pushes;
        return true;
    };
    coalescer.setPaused(true);
    EXPECT_TRUE(coalescer.paused());
    EXPECT_FALSE(coalescer.notify(push));
    EXPECT_FALSE(coalescer.pending());
    coalescer.setPaused(false);
    EXPECT_TRUE(coalescer.notify(push));
    EXPECT_EQ(pushes, 1);
}

TEST(WakeCoalescerTest, ResetClearsAPendingWakeAndAPause)
{
    Core::WakeCoalescer coalescer;
    EXPECT_TRUE(coalescer.notify([] { return true; }));
    coalescer.setPaused(true);
    coalescer.reset();
    EXPECT_FALSE(coalescer.pending());
    EXPECT_FALSE(coalescer.paused());
}

TEST(WakeCoalescerTest, ManyPublishersAtOncePushOneWake)
{
    Core::WakeCoalescer coalescer;
    std::atomic<int> pushes{0};
    constexpr int THREADS = 8;
    constexpr int NOTIFIES = 1000;
    {
        std::vector<std::jthread> publishers;
        publishers.reserve(THREADS);
        for (int t = 0; t < THREADS; ++t)
        {
            publishers.emplace_back(
                [&coalescer, &pushes]
                {
                    for (int i = 0; i < NOTIFIES; ++i)
                    {
                        static_cast<void>(coalescer.notify(
                            [&pushes]
                            {
                                pushes.fetch_add(1);
                                return true;
                            }));
                    }
                });
        }
    }
    EXPECT_EQ(pushes.load(), 1) << "nothing took the wake, so every other publication was coalesced into it";
}

// Every publication is shown: a publisher commits, then notifies; the loop takes the wake, then reads.
// Whatever the interleaving, the last read after the publishers stop sees the last commit, and the
// queue never holds more than one wake.
TEST(WakeCoalescerTest, NoPublicationIsLeftWithoutAWake)
{
    Core::WakeCoalescer coalescer;
    std::atomic<int> published{0};
    std::atomic<int> queued{0};
    std::atomic<int> mostQueued{0};
    std::atomic<bool> publishing{true};
    int seen = 0;
    const auto push = [&queued, &mostQueued]
    {
        const int now = queued.fetch_add(1) + 1;
        int most = mostQueued.load();
        while (now > most && !mostQueued.compare_exchange_weak(most, now))
        {
        }
        return true;
    };
    std::jthread loop(
        [&]
        {
            const auto drain = [&]
            {
                while (queued.load() > 0)
                {
                    queued.fetch_sub(1);
                    coalescer.taken();
                    seen = published.load(); // the frame reads after the drain
                }
            };
            while (publishing.load())
            {
                drain();
                std::this_thread::yield();
            }
            drain();
        });
    {
        constexpr int PUBLISHERS = 4;
        std::vector<std::jthread> publishers;
        publishers.reserve(PUBLISHERS);
        for (int t = 0; t < PUBLISHERS; ++t)
        {
            publishers.emplace_back(
                [&]
                {
                    for (int i = 0; i < 2000; ++i)
                    {
                        published.fetch_add(1); // the commit
                        static_cast<void>(coalescer.notify(push));
                    }
                });
        }
    }
    publishing = false;
    loop.join();
    EXPECT_EQ(seen, published.load()) << "the last publication's wake brought a read that saw it";
    EXPECT_EQ(mostQueued.load(), 1) << "never more than one wake event queued";
}

} // namespace
