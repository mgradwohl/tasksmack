/// @file test_BackgroundTask.cpp
/// @brief App::BackgroundTask (#2139), the worker slot shared by the App views that wait on a slow
/// platform call: start/poll, the stop token (a new one per start), no restart while busy, the
/// destructor requesting a stop and waiting, work that throws (a std::exception or anything else)
/// turned into the Failed result on the worker and never rethrown by poll(), and an adopted answer
/// (a promise) dropped rather than waited for.

#include "App/Panels/BackgroundTask.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace App
{
namespace
{

struct FakeResult
{
    int value = 0;
    std::string error;
};

FakeResult failed(std::string detail)
{
    return {.value = -1, .error = std::move(detail)};
}

/// The detail of work whose Failed result can't be built on the worker (as bad_alloc would).
constexpr const char* UNBUILDABLE = "unbuildable";

FakeResult failedUnlessUnbuildable(std::string detail)
{
    if (detail == UNBUILDABLE)
    {
        throw std::bad_alloc();
    }
    return failed(std::move(detail));
}

using Task = BackgroundTask<FakeResult>;
constexpr std::string_view THREAD_NAME = "ts-test-task";

/// Waits (at most a few seconds, so a broken stop never hangs the suite) until @p token asks to stop.
bool waitForStop(const std::stop_token& token)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!token.stop_requested())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

TEST(BackgroundTaskTest, StartsWorkAndPollsItsResult)
{
    Task task(THREAD_NAME, &failed);
    EXPECT_FALSE(task.busy());
    EXPECT_FALSE(task.poll().has_value()); // nothing started
    EXPECT_FALSE(task.start([] { return FakeResult{.value = 7, .error = {}}; }).has_value());
    EXPECT_TRUE(task.busy()); // until its result is taken in
    const std::optional<FakeResult> result = task.poll(true);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(FakeResult{}).value, 7);
    EXPECT_FALSE(task.busy());
    EXPECT_FALSE(task.poll(true).has_value()); // taken once
}

TEST(BackgroundTaskTest, PollDoesNotWaitForWorkStillRunning)
{
    Task task(THREAD_NAME, &failed);
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    ASSERT_FALSE(task.start(
                         [opened]
                         {
                             opened.wait();
                             return FakeResult{.value = 3, .error = {}};
                         })
                     .has_value());
    EXPECT_FALSE(task.poll().has_value());
    EXPECT_TRUE(task.busy());
    gate.set_value();
    EXPECT_EQ(task.poll(true).value_or(FakeResult{}).value, 3);
}

TEST(BackgroundTaskTest, DoesNotRestartWhileBusy)
{
    Task task(THREAD_NAME, &failed);
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    std::atomic<bool> secondRan{false};
    ASSERT_FALSE(task.start(
                         [opened]
                         {
                             opened.wait();
                             return FakeResult{.value = 1, .error = {}};
                         })
                     .has_value());
    // Neither replaced nor waited for: the second start returns at once, and its work never runs.
    EXPECT_FALSE(task.start(
                         [&secondRan]
                         {
                             secondRan = true;
                             return FakeResult{.value = 2, .error = {}};
                         })
                     .has_value());
    gate.set_value();
    EXPECT_EQ(task.poll(true).value_or(FakeResult{}).value, 1);
    EXPECT_FALSE(secondRan.load());
}

TEST(BackgroundTaskTest, RequestStopReachesTheWorkThroughItsToken)
{
    Task task(THREAD_NAME, &failed);
    ASSERT_FALSE(
        task.start([](const std::stop_token& token) { return FakeResult{.value = waitForStop(token) ? 1 : 0, .error = {}}; }).has_value());
    task.requestStop();
    EXPECT_EQ(task.poll(true).value_or(FakeResult{}).value, 1);
}

TEST(BackgroundTaskTest, EachStartHasANewStopToken)
{
    Task task(THREAD_NAME, &failed);
    ASSERT_FALSE(
        task.start([](const std::stop_token& token) { return FakeResult{.value = waitForStop(token) ? 1 : 0, .error = {}}; }).has_value());
    task.requestStop();
    ASSERT_TRUE(task.poll(true).has_value());
    // The stop of the earlier work does not carry over.
    ASSERT_FALSE(task.start([](const std::stop_token& token) { return FakeResult{.value = token.stop_requested() ? 1 : 0, .error = {}}; })
                     .has_value());
    EXPECT_EQ(task.poll(true).value_or(FakeResult{.value = -2, .error = {}}).value, 0);
}

TEST(BackgroundTaskTest, DestructorRequestsAStopAndWaits)
{
    std::atomic<bool> finished{false};
    {
        Task task(THREAD_NAME, &failed);
        ASSERT_FALSE(task.start(
                             [&finished](const std::stop_token& token)
                             {
                                 const bool stopped = waitForStop(token);
                                 finished = true;
                                 return FakeResult{.value = stopped ? 1 : 0, .error = {}};
                             })
                         .has_value());
    }
    EXPECT_TRUE(finished.load());
}

TEST(BackgroundTaskTest, DestructorWaitsForWorkThatIgnoresTheStop)
{
    std::atomic<bool> finished{false};
    {
        Task task(THREAD_NAME, &failed);
        ASSERT_FALSE(task.start(
                             [&finished]
                             {
                                 std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                 finished = true;
                                 return FakeResult{};
                             })
                         .has_value());
    }
    EXPECT_TRUE(finished.load());
}

TEST(BackgroundTaskTest, WaitLeavesTheResultForPoll)
{
    Task task(THREAD_NAME, &failed);
    ASSERT_FALSE(task.start([] { return FakeResult{.value = 5, .error = {}}; }).has_value());
    task.wait();
    EXPECT_TRUE(task.busy());
    EXPECT_EQ(task.poll().value_or(FakeResult{}).value, 5);
}

TEST(BackgroundTaskTest, WorkThrowingAStdExceptionIsTheFailedResult)
{
    Task task(THREAD_NAME, &failed);
    ASSERT_FALSE(task.start([]() -> FakeResult { throw std::runtime_error("boom"); }).has_value());
    const std::optional<FakeResult> result = task.poll(true);
    ASSERT_TRUE(result.has_value());
    const FakeResult failure = result.value_or(FakeResult{});
    EXPECT_EQ(failure.value, -1);
    EXPECT_EQ(failure.error, "boom");
}

TEST(BackgroundTaskTest, WorkThrowingANonStdExceptionIsTheFailedResult)
{
    Task task(THREAD_NAME, &failed);
    // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally non-std: the catch-all path
    ASSERT_FALSE(task.start([]() -> FakeResult { throw 42; }).has_value());
    const FakeResult failure = task.poll(true).value_or(FakeResult{});
    EXPECT_EQ(failure.value, -1);
    EXPECT_EQ(failure.error, "Unknown error");

    Task named(THREAD_NAME, &failed, "unknown error");
    // NOLINTNEXTLINE(hicpp-exception-baseclass,bugprone-std-exception-baseclass) - intentionally non-std: the catch-all path
    ASSERT_FALSE(named.start([]() -> FakeResult { throw 42; }).has_value());
    EXPECT_EQ(named.poll(true).value_or(FakeResult{}).error, "unknown error");
}

TEST(BackgroundTaskTest, AFailedResultThatCantBeBuiltOnTheWorkerIsNotRethrown)
{
    // The worker's Failed result throws (bad_alloc); that exception crosses through the future, and
    // poll() takes it in as the unknown-error Failed result without rethrowing or reading it (#1685).
    Task task(THREAD_NAME, &failedUnlessUnbuildable);
    ASSERT_FALSE(task.start([]() -> FakeResult { throw std::runtime_error(UNBUILDABLE); }).has_value());
    std::optional<FakeResult> result;
    EXPECT_NO_THROW(result = task.poll(true));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value_or(FakeResult{}).error, "Unknown error");
    EXPECT_FALSE(task.busy());
}

TEST(BackgroundTaskTest, AnAdoptedAnswerIsPolledLikeWork)
{
    Task task(THREAD_NAME, &failed);
    std::promise<FakeResult> answer;
    task.adopt(answer.get_future());
    EXPECT_TRUE(task.busy());
    EXPECT_FALSE(task.poll().has_value());
    answer.set_value(FakeResult{.value = 9, .error = {}});
    EXPECT_EQ(task.poll().value_or(FakeResult{}).value, 9);
    EXPECT_FALSE(task.busy());
}

TEST(BackgroundTaskTest, AnAdoptedAnswerIsDroppedRatherThanWaitedFor)
{
    auto answer = std::make_shared<std::promise<FakeResult>>();
    {
        Task task(THREAD_NAME, &failed);
        task.adopt(answer->get_future());
        task.wait(); // would never return if it waited: nothing answers
        EXPECT_FALSE(task.busy());
        task.adopt(std::make_shared<std::promise<FakeResult>>()->get_future()); // broken at once
        EXPECT_EQ(task.poll(true).value_or(FakeResult{}).error, "Unknown error");
    }
    // A late answer to the dropped one is harmless.
    EXPECT_NO_THROW(answer->set_value(FakeResult{}));
}

TEST(BackgroundTaskTest, WorkInFlightMovesWithTheTask)
{
    std::promise<void> gate;
    const std::shared_future<void> opened = gate.get_future().share();
    Task task(THREAD_NAME, &failed);
    ASSERT_FALSE(task.start(
                         [opened](const std::stop_token& token)
                         {
                             opened.wait();
                             return FakeResult{.value = token.stop_requested() ? 1 : 2, .error = {}};
                         })
                     .has_value());
    Task moved(std::move(task));
    EXPECT_TRUE(moved.busy());
    moved.requestStop(); // the moved-to task holds the work's stop source
    gate.set_value();
    EXPECT_EQ(moved.poll(true).value_or(FakeResult{}).value, 1);
}

} // namespace
} // namespace App
