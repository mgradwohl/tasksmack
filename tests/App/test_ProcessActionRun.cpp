/// @file test_ProcessActionRun.cpp
/// @brief App::ProcessActionRun (#2305): one single-process action at a time on a worker, its result
/// taken in on the caller's thread, never on the worker's.

#include "App/Panels/ProcessActionRun.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <future>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace App
{
namespace
{

TEST(ProcessActionRunTest, RunsTheWorkOnTheWorkerAndFinishesOnTheCallersThread)
{
    ProcessActionRun run;
    std::thread::id workThread;
    std::thread::id finishThread;
    std::optional<Platform::ProcessActionResult> finished;
    ASSERT_TRUE(run.start(
        "Terminating PID 42...",
        [&workThread]
        {
            workThread = std::this_thread::get_id();
            return Platform::ProcessActionResult::ok();
        },
        [&](const Platform::ProcessActionResult& result)
        {
            finishThread = std::this_thread::get_id();
            finished = result;
        }));
    EXPECT_TRUE(run.update(/*wait=*/true));
    ASSERT_TRUE(finished.has_value());
    EXPECT_TRUE(finished->success);
    EXPECT_NE(workThread, std::this_thread::get_id()) << "the platform call never runs on the UI thread";
    EXPECT_EQ(finishThread, std::this_thread::get_id()) << "the result is taken in on the UI thread";
    EXPECT_FALSE(run.busy());
    EXPECT_TRUE(run.pendingText().empty());
}

TEST(ProcessActionRunTest, ShowsItsPendingTextAndRefusesASecondActionWhileOneRuns)
{
    ProcessActionRun run;
    std::latch release{1};
    int finishes = 0;
    ASSERT_TRUE(run.start(
        "Killing PID 7...",
        [&release]
        {
            release.wait();
            return Platform::ProcessActionResult::ok();
        },
        [&finishes](const Platform::ProcessActionResult& /*result*/) { ++finishes; }));
    EXPECT_TRUE(run.busy());
    EXPECT_EQ(run.pendingText(), "Killing PID 7...");
    EXPECT_FALSE(run.update()) << "not finished yet";
    EXPECT_FALSE(run.start("Another", [] { return Platform::ProcessActionResult::ok(); }, nullptr));

    release.count_down();
    EXPECT_TRUE(run.update(/*wait=*/true));
    EXPECT_EQ(finishes, 1);
}

TEST(ProcessActionRunTest, WorkThatThrowsFinishesAsAFailedResult)
{
    ProcessActionRun run;
    std::optional<Platform::ProcessActionResult> finished;
    ASSERT_TRUE(run.start(
        "Resuming PID 9...",
        []() -> Platform::ProcessActionResult { throw std::runtime_error("the platform call failed"); },
        [&finished](const Platform::ProcessActionResult& result) { finished = result; }));
    EXPECT_TRUE(run.update(/*wait=*/true));
    ASSERT_TRUE(finished.has_value());
    EXPECT_FALSE(finished->success);
    EXPECT_EQ(finished->errorMessage, "the platform call failed");
}

TEST(ProcessActionRunTest, DestroyingItWaitsForTheActionInFlight)
{
    std::promise<void> entered;
    std::latch release{1};
    bool workFinished = false;
    {
        ProcessActionRun run;
        ASSERT_TRUE(run.start(
            "Suspending PID 3...",
            [&]
            {
                entered.set_value();
                release.wait();
                workFinished = true;
                return Platform::ProcessActionResult::ok();
            },
            nullptr));
        entered.get_future().wait();
        release.count_down();
    } // waits for the worker, so the actions it calls may be destroyed right after
    EXPECT_TRUE(workFinished);
}

} // namespace
} // namespace App
