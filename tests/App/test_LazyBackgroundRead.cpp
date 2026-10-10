/// @file test_LazyBackgroundRead.cpp
/// @brief App::Detail::LazyBackgroundRead, the lazy worker read shared by Process Details' Connections,
/// Modules and Open files sections: due only while drawn open and at the refresh interval (or, on
/// demand, once per request), one read at a time, a read for a previous selection, another target or a
/// closed on-demand section dropped, and a read that throws (a std::exception or anything else) turned
/// into the Failed result on the worker.

#include "App/Panels/LazyBackgroundRead.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <future>
#include <optional>
#include <stdexcept>
#include <string>
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

} // namespace
} // namespace App::Detail
