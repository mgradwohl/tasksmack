/// @file test_LazyBackgroundRead.cpp
/// @brief App::Detail::LazyBackgroundRead, the lazy worker read shared by Process Details' Connections,
/// Modules and Open files sections: due only while drawn open and at the refresh interval (or, on
/// demand, once per request), one read at a time, a read for a previous selection, another target or a
/// closed on-demand section dropped (and, for a selection change or a close, stopped: #2404), a
/// read that throws (a std::exception or anything else) turned into the Failed result on the worker,
/// and the "Authenticate to read" request (#2309) the sections share (#2567): taken once, withdrawn by
/// a selection change, offered only while the reader can read as an administrator. Since #3343 each read
/// runs on a Domain::Worker: shutdown with a read queued, and a read that cannot start.

#include "App/Panels/LazyBackgroundRead.h"
#include "Domain/AllocationFailureHook.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace App::Detail
{
namespace
{

constexpr Platform::ProcessTarget TARGET{.pid = 10, .startTimeTicks = 20};
constexpr int REFRESH_MS = 1000;

struct FakeResult
{
    int value = 0;
    std::string error;
};

FakeResult failed(std::string detail)
{
    return {.value = -1, .error = std::move(detail)};
}

using Read = LazyBackgroundRead<FakeResult>;

TEST(LazyBackgroundReadTest, DueOnlyWhenDrawnOpenAndAtTheInterval)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    EXPECT_FALSE(read.due(10.0F)); // never drawn open
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F)); // first open: at once
    EXPECT_FALSE(read.start(TARGET, [](const Platform::ProcessTarget& t) { return FakeResult{.value = t.pid, .error = {}}; }).has_value());

    read.markDrawnOpen();
    EXPECT_FALSE(read.due(5.0F)); // one in flight
    const std::optional<FakeResult> result = read.takeFinished(TARGET, true);
    EXPECT_EQ(result.value_or(FakeResult{}).value, 10);
    EXPECT_FALSE(read.inFlight());

    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F)); // 5 s have passed since it started
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{}; }));
    static_cast<void>(read.takeFinished(TARGET, true));
    read.markDrawnOpen();
    EXPECT_FALSE(read.due(0.5F)); // half the interval
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.5F));
}

// #2734: rearm() makes the next read due at once, inside the interval, and keeps the drawn-open flag,
// unlike reset(), so the frame that saw the new target starts its read.
TEST(LazyBackgroundReadTest, RearmMakesTheNextReadDueAtOnceAndKeepsTheDrawnFlag)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{}; }));
    static_cast<void>(read.takeFinished(TARGET, true));

    read.markDrawnOpen();
    read.rearm();
    EXPECT_TRUE(read.due(0.0F)); // well inside the interval, and the flag set before rearm() still counts

    read.rearm(); // not drawn open: still nothing starts
    EXPECT_FALSE(read.due(0.0F));
}

TEST(LazyBackgroundReadTest, DropsAReadForAPreviousSelection)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{.value = 1, .error = {}}; }));
    read.reset();
    EXPECT_FALSE(read.takeFinished(TARGET, true).has_value());
    EXPECT_FALSE(read.due(0.0F)); // reset() drops the open frame too
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F)); // and the next read is due at once
}

TEST(LazyBackgroundReadTest, AReadForAnotherTargetIsDroppedAndReadAgain)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{}; }));
    EXPECT_FALSE(read.takeFinished({.pid = 11, .startTimeTicks = 20}, true).has_value());
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F)); // due again without waiting for the interval
}

TEST(LazyBackgroundReadTest, AThrowingReadIsTheFailedResult)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) -> FakeResult { throw std::runtime_error("boom"); }));
    const std::optional<FakeResult> result = read.takeFinished(TARGET, true);
    ASSERT_TRUE(result.has_value());
    const FakeResult failure = result.value_or(FakeResult{});
    EXPECT_EQ(failure.value, -1);
    EXPECT_EQ(failure.error, "boom");
}

TEST(LazyBackgroundReadTest, AReadThrowingANonStdExceptionIsTheFailedResult)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally non-std: the catch-all path
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) -> FakeResult { throw 42; }));
    const std::optional<FakeResult> result = read.takeFinished(TARGET, true);
    ASSERT_TRUE(result.has_value());
    const FakeResult failure = result.value_or(FakeResult{});
    EXPECT_EQ(failure.value, -1);
    EXPECT_EQ(failure.error, "unknown error");
}

TEST(LazyBackgroundReadTest, DoesNotWaitForAReadStillRunning)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET,
                                 [opened](const Platform::ProcessTarget&)
                                 {
                                     opened.wait();
                                     return FakeResult{.value = 7, .error = {}};
                                 }));
    EXPECT_FALSE(read.takeFinished(TARGET, false).has_value());
    EXPECT_TRUE(read.inFlight());
    gate.set_value();
    EXPECT_EQ(read.takeFinished(TARGET, true).value_or(FakeResult{}).value, 7);
}

TEST(LazyBackgroundReadTest, OnDemandReadsOncePerRequestOnlyWhileOpen)
{
    Read read(Read::ON_DEMAND, "ts-test-read", &failed);
    read.markDrawnOpen();
    EXPECT_FALSE(read.due(100.0F)); // open, but nothing asked: never on a timer
    read.request();
    EXPECT_TRUE(read.requestOutstanding());
    EXPECT_FALSE(read.due(0.0F)); // asked, but not drawn open this frame: withdrawn
    EXPECT_FALSE(read.requestOutstanding());

    read.request();
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{.value = 3, .error = {}}; }));
    EXPECT_TRUE(read.requestOutstanding());
    EXPECT_EQ(read.takeFinished(TARGET, true).value_or(FakeResult{}).value, 3);
    EXPECT_FALSE(read.requestOutstanding());
    read.markDrawnOpen();
    EXPECT_FALSE(read.due(100.0F)); // one read per request
}

TEST(LazyBackgroundReadTest, OnDemandClosingMidReadDropsIt)
{
    Read read(Read::ON_DEMAND, "ts-test-read", &failed);
    read.request();
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{.value = 4, .error = {}}; }));
    EXPECT_FALSE(read.due(0.0F)); // not drawn open: closed
    EXPECT_FALSE(read.requestOutstanding());
    EXPECT_FALSE(read.takeFinished(TARGET, true).has_value());
}

// #2404: a selection change stops the read in flight (a password prompt's wait polls its stop token),
// so the new process's read is due as soon as the stopped one is taken in, not after the prompt.
TEST(LazyBackgroundReadTest, ResetStopsTheReadInFlight)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET,
                                 [](const Platform::ProcessTarget&, const std::stop_token& stop)
                                 {
                                     while (!stop.stop_requested())
                                     {
                                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                     }
                                     return FakeResult{.value = 5, .error = {}};
                                 }));
    read.reset();
    EXPECT_FALSE(read.takeFinished(TARGET, true).has_value()); // returns: the read was stopped, and is dropped
    EXPECT_FALSE(read.inFlight());
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F));
}

// #2404: closing an on-demand section stops its read in flight too.
TEST(LazyBackgroundReadTest, OnDemandClosingStopsTheReadInFlight)
{
    Read read(Read::ON_DEMAND, "ts-test-read", &failed);
    read.request();
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET,
                                 [](const Platform::ProcessTarget&, const std::stop_token& stop)
                                 {
                                     while (!stop.stop_requested())
                                     {
                                         std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                     }
                                     return FakeResult{.value = 6, .error = {}};
                                 }));
    EXPECT_FALSE(read.due(0.0F)); // not drawn open: closed
    EXPECT_FALSE(read.takeFinished(TARGET, true).has_value());
    EXPECT_FALSE(read.inFlight());
    read.request();
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(0.0F));
}

// #3343 shutdown: a section destroyed with its read just started (still on its worker's queue, or running)
// stops the read and waits for it, so nothing it uses is gone while it runs.
TEST(LazyBackgroundReadTest, DestroyedWithAReadQueuedItStopsTheReadAndWaits)
{
    std::atomic<int> stopped{0};
    constexpr int RUNS = 50;
    for (int run = 0; run < RUNS; ++run)
    {
        Read read(REFRESH_MS, "ts-test-read", &failed);
        read.markDrawnOpen();
        ASSERT_TRUE(read.due(0.0F));
        ASSERT_FALSE(read.start(TARGET,
                                [&stopped](const Platform::ProcessTarget&, const std::stop_token& stop)
                                {
                                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                                    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
                                    {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                    }
                                    if (stop.stop_requested())
                                    {
                                        stopped.fetch_add(1);
                                    }
                                    return FakeResult{};
                                })
                         .has_value());
    }
    EXPECT_EQ(stopped.load(), RUNS);
}

// #3343: a read that throws after a selection change is dropped like any read for the previous selection,
// and the worker it threw on doesn't hold up the next read.
TEST(LazyBackgroundReadTest, AThrowingReadForAPreviousSelectionIsDroppedAndTheNextReadRuns)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    static_cast<void>(read.start(TARGET, [](const Platform::ProcessTarget&) -> FakeResult { throw std::runtime_error("boom"); }));
    read.reset();
    EXPECT_FALSE(read.takeFinished(TARGET, true).has_value());
    EXPECT_FALSE(read.inFlight());
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    ASSERT_FALSE(read.start(TARGET, [](const Platform::ProcessTarget& t) { return FakeResult{.value = t.pid, .error = {}}; }).has_value());
    EXPECT_EQ(read.takeFinished(TARGET, true).value_or(FakeResult{}).value, TARGET.pid);
}

// #3343: a read that can't start (no memory for its worker) is the Failed result at once, with nothing in
// flight, so the section shows it and the next refresh tries again. Skipped where the hook is compiled out
// (Windows, sanitizer builds: AllocationFailureHook.h); the Linux debug and release builds run it.
TEST(LazyBackgroundReadTest, AReadThatCannotStartIsTheFailedResultAndTheNextRefreshRetries)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.markDrawnOpen();
    ASSERT_TRUE(read.due(0.0F));
    std::optional<FakeResult> failure;
    {
        const TestSupport::FailAllocationAfter failNext(0);
        failure = read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{.value = 1, .error = {}}; });
    }
    ASSERT_TRUE(failure.has_value());
    EXPECT_EQ(failure.value_or(FakeResult{}).value, -1);
    EXPECT_FALSE(read.inFlight());
    read.markDrawnOpen();
    EXPECT_TRUE(read.due(static_cast<float>(REFRESH_MS) / 1000.0F)); // the next refresh
    ASSERT_FALSE(read.start(TARGET, [](const Platform::ProcessTarget&) { return FakeResult{.value = 2, .error = {}}; }).has_value());
    EXPECT_EQ(read.takeFinished(TARGET, true).value_or(FakeResult{}).value, 2);
#endif
}

// #2567: the "Authenticate to read" press (#2309) the four sections share. The view's next update()
// takes it, once: a second update() finds nothing, and so does one after a press that was never made.
TEST(LazyBackgroundReadTest, AnAuthorizeRequestIsTakenOnce)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    EXPECT_FALSE(read.takeAuthorizeRequest()); // nothing pressed
    read.requestAuthorized();
    read.requestAuthorized(); // pressed twice before an update(): one request
    EXPECT_TRUE(read.takeAuthorizeRequest());
    EXPECT_FALSE(read.takeAuthorizeRequest()); // taken
}

// #2567: a selection change withdraws a press not taken yet -- the prompt would have been for the
// previous process -- as it stops and drops the read in flight.
TEST(LazyBackgroundReadTest, ResetWithdrawsAnAuthorizeRequestNotTakenYet)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    read.requestAuthorized();
    read.reset();
    EXPECT_FALSE(read.takeAuthorizeRequest());
    read.requestAuthorized(); // pressed again for the new process: taken as usual
    EXPECT_TRUE(read.takeAuthorizeRequest());
}

// #2567: whether the reader can read as an administrator gates the offer. It is the reader's property,
// recorded by every update(), not the selection's: a reset() leaves it as last recorded.
TEST(LazyBackgroundReadTest, CanAuthorizeIsAsLastRecordedAndSurvivesReset)
{
    Read read(REFRESH_MS, "ts-test-read", &failed);
    EXPECT_FALSE(read.canAuthorize()); // until a reader says so
    read.setCanAuthorize(true);
    EXPECT_TRUE(read.canAuthorize());
    read.reset();
    EXPECT_TRUE(read.canAuthorize());
    read.setCanAuthorize(false); // the reader changed (a null reader, or the helper went away)
    EXPECT_FALSE(read.canAuthorize());
}

} // namespace
} // namespace App::Detail
