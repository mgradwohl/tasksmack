/// @file test_ProcessBatchRun.cpp
/// @brief A confirmed batch action or batch priority change runs on a worker (#2142): start*() returns
/// at once with every target still to do, the toolbar's progress line counts them as they finish, the
/// outcome is taken in on a later poll(), a stop (shutdown) acts on no further target, and work that
/// throws comes back as the Failed outcome. Also runBatch()'s stop token and progress count, directly.

#include "App/Panels/ProcessBatchAction.h"
#include "App/Panels/ProcessBatchRun.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <future>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace App::ProcessBatch
{
namespace
{

using Detail::ProcessAction;

constexpr std::int32_t OWN_PID = 4000;

/// The synthetic scale of #2142's report: Ctrl+A over 5,000 processes.
constexpr std::size_t LARGE_SELECTION = 5000;

/// Counts the calls made on the worker, and holds the first one until open() (when gated): so a test
/// can see the batch started and still running. Throws from every call when told to.
class GatedProcessActions : public TestMocks::MockProcessActions
{
  public:
    explicit GatedProcessActions(bool gated = true)
    {
        if (!gated)
        {
            open();
        }
    }

    void open()
    {
        if (!m_Opened.exchange(true))
        {
            m_Gate.set_value();
        }
    }

    /// Waits (at most a few seconds, so a broken run never hangs the suite) until the first call is held.
    [[nodiscard]] bool waitUntilEntered() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!m_Entered.load())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }

    [[nodiscard]] std::size_t calls() const
    {
        return m_Calls.load();
    }

    [[nodiscard]] std::int32_t lastPid() const
    {
        return m_LastPid.load();
    }

    std::atomic<bool> throwOnCall{false};

    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& target) override
    {
        return record(target);
    }
    [[nodiscard]] Platform::ProcessActionResult setPriority(const Platform::ProcessTarget& target, std::int32_t nice) override
    {
        static_cast<void>(nice);
        return record(target);
    }

  private:
    Platform::ProcessActionResult record(const Platform::ProcessTarget& target)
    {
        if (m_Calls.fetch_add(1) == 0)
        {
            m_Entered.store(true);
            m_GateFuture.wait();
        }
        m_LastPid.store(target.pid);
        if (throwOnCall.load())
        {
            throw std::runtime_error("boom");
        }
        return Platform::ProcessActionResult::ok();
    }

    std::promise<void> m_Gate;
    std::shared_future<void> m_GateFuture = m_Gate.get_future().share();
    std::atomic<bool> m_Opened{false};
    std::atomic<bool> m_Entered{false};
    std::atomic<std::size_t> m_Calls{0};
    std::atomic<std::int32_t> m_LastPid{0};
};

std::vector<BatchTarget> numberedTargets(std::size_t count)
{
    std::vector<BatchTarget> targets;
    targets.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto pid = static_cast<std::int32_t>(100 + i);
        targets.push_back({.target = {.pid = pid, .startTimeTicks = static_cast<std::uint64_t>(pid) * 10U},
                           .name = std::format("proc{}", i),
                           .priority = {}});
    }
    return targets;
}

// ========== The worker ==========

TEST(ProcessBatchRunTest, AConfirmedBatchPriorityReturnsAtOnceWithItsWorkStillPending)
{
    GatedProcessActions actions; // the first process is held until open()
    BatchRun run;
    EXPECT_FALSE(run.busy());

    // The frame that confirmed it: start returns while the worker is still on its first process.
    EXPECT_FALSE(run.startPriority(actions, numberedTargets(LARGE_SELECTION), 10, OWN_PID).has_value());
    ASSERT_TRUE(actions.waitUntilEntered());
    EXPECT_TRUE(run.busy());
    EXPECT_FALSE(run.poll().has_value()); // a later frame: still running, nothing to take in yet
    EXPECT_EQ(run.statusText(), "Set priority: 0 of 5000 processes...");

    actions.open();
    const std::optional<BatchRunOutcome> outcome = run.poll(true);
    ASSERT_TRUE(outcome.has_value());
    const BatchRunOutcome got = outcome.value_or(BatchRunOutcome{});
    EXPECT_FALSE(run.busy());
    EXPECT_EQ(actions.calls(), LARGE_SELECTION);
    EXPECT_TRUE(got.anySucceeded);
    EXPECT_TRUE(got.message.ok);
    EXPECT_EQ(got.message.text,
              formatBatchPriorityResultMessage(10, {.attempted = LARGE_SELECTION, .succeeded = LARGE_SELECTION, .firstFailures = {}}).text);
}

TEST(ProcessBatchRunTest, AConfirmedBatchActionRunsOnTheWorkerWithTaskSmackItselfLast)
{
    GatedProcessActions actions(false);
    BatchRun run;
    std::vector<BatchTarget> targets = numberedTargets(3);
    targets.insert(targets.begin(), {.target = {.pid = OWN_PID, .startTimeTicks = 1}, .name = "tasksmack", .priority = {}});

    EXPECT_FALSE(run.startAction(actions, ProcessAction::Kill, std::move(targets), OWN_PID).has_value());
    const std::optional<BatchRunOutcome> outcome = run.poll(true);
    ASSERT_TRUE(outcome.has_value());
    const BatchRunOutcome got = outcome.value_or(BatchRunOutcome{});
    EXPECT_EQ(actions.calls(), 4U);
    EXPECT_EQ(actions.lastPid(), OWN_PID);
    EXPECT_EQ(got.message.text, "Kill sent to 4 processes");
    EXPECT_TRUE(got.anySucceeded);
    EXPECT_EQ(run.statusText(), "Kill: 4 of 4 processes...");
}

TEST(ProcessBatchRunTest, NoSecondBatchStartsWhileOneRuns)
{
    GatedProcessActions actions;
    GatedProcessActions other(false);
    BatchRun run;
    EXPECT_FALSE(run.startAction(actions, ProcessAction::Kill, numberedTargets(2), OWN_PID).has_value());
    ASSERT_TRUE(actions.waitUntilEntered());

    EXPECT_FALSE(run.startPriority(other, numberedTargets(2), 0, OWN_PID).has_value());
    EXPECT_EQ(run.statusText(), "Kill: 0 of 2 processes..."); // still the first batch's
    actions.open();
    const std::optional<BatchRunOutcome> outcome = run.poll(true);
    ASSERT_TRUE(outcome.has_value());
    const BatchRunOutcome got = outcome.value_or(BatchRunOutcome{});
    EXPECT_EQ(got.message.text, "Kill sent to 2 processes");
    EXPECT_EQ(other.calls(), 0U);
}

TEST(ProcessBatchRunTest, ShutdownActsOnNoFurtherProcess)
{
    GatedProcessActions actions;
    BatchRun run;
    EXPECT_FALSE(run.startPriority(actions, numberedTargets(LARGE_SELECTION), 5, OWN_PID).has_value());
    ASSERT_TRUE(actions.waitUntilEntered());

    // The stop is requested at once; the held process is let go only well after, so the run sees the
    // stop before its next one.
    std::jthread opener(
        [&actions]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            actions.open();
        });
    run.shutdown();
    EXPECT_EQ(actions.calls(), 1U);
    opener.join();
}

TEST(ProcessBatchRunTest, WorkThatThrowsComesBackAsTheFailedOutcome)
{
    GatedProcessActions actions(false);
    actions.throwOnCall = true;
    BatchRun run;
    EXPECT_FALSE(run.startAction(actions, ProcessAction::Kill, numberedTargets(2), OWN_PID).has_value());
    const std::optional<BatchRunOutcome> outcome = run.poll(true);
    ASSERT_TRUE(outcome.has_value());
    const BatchRunOutcome got = outcome.value_or(BatchRunOutcome{});
    EXPECT_FALSE(got.message.ok);
    EXPECT_FALSE(got.anySucceeded);
    EXPECT_EQ(got.message.text, "Could not run the batch: boom");
}

TEST(ProcessBatchRunTest, PollWithNothingStartedHasNothing)
{
    BatchRun run;
    EXPECT_FALSE(run.poll().has_value());
    run.shutdown(); // nothing to stop
    EXPECT_FALSE(run.busy());
}

// ========== runBatch()'s stop token and count ==========

TEST(ProcessBatchRunTest, RunBatchStopsBeforeItsNextTargetOnceAStopIsRequested)
{
    std::stop_source stop;
    std::atomic<std::size_t> done{0};
    std::vector<BatchTarget> targets = numberedTargets(4);
    targets.push_back({.target = {.pid = OWN_PID, .startTimeTicks = 1}, .name = "tasksmack", .priority = {}});
    std::vector<std::int32_t> acted;
    const BatchResult result = runBatch(
        targets,
        OWN_PID,
        [&](const Platform::ProcessTarget& target)
        {
            acted.push_back(target.pid);
            if (acted.size() == 2)
            {
                static_cast<void>(stop.request_stop());
            }
            return Platform::ProcessActionResult::ok();
        },
        stop.get_token(),
        &done);
    EXPECT_EQ(result.attempted, 2U);
    EXPECT_EQ(result.succeeded, 2U);
    EXPECT_EQ(done.load(), 2U);
    EXPECT_EQ(acted, (std::vector<std::int32_t>{100, 101})); // TaskSmack itself, last, was never reached
}

TEST(ProcessBatchRunTest, RunBatchCountsEveryTargetItFinishes)
{
    std::atomic<std::size_t> done{0};
    const BatchResult result = runBatch(
        numberedTargets(3),
        OWN_PID,
        [](const Platform::ProcessTarget& target)
        { return target.pid == 101 ? Platform::ProcessActionResult::error("denied") : Platform::ProcessActionResult::ok(); },
        {},
        &done);
    EXPECT_EQ(result.attempted, 3U);
    EXPECT_EQ(result.failed(), 1U);
    EXPECT_EQ(done.load(), 3U); // a failure is finished too
}

} // namespace
} // namespace App::ProcessBatch
