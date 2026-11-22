// Tests for Domain::DrainTrigger (#3790): the drain worker KeyedLookup and ProcessIconCache share. One drain at a
// time; a wake() while one waits merges into it and one while it runs queues the next, so an item queued during a
// drain is never left unrun; a drain that throws fails the worker; the destructor stops a drain at its next check.

#include "Domain/DrainTrigger.h"
#include "Domain/Worker.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

namespace Domain
{
namespace
{

constexpr auto DRAIN_WAIT = std::chrono::seconds(5);

/// An owner as the rule describes: a queue under its own lock, an item queued and then wake(), and a drain that
/// pops one item at a time.
struct Owner
{
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<int> queue;
    std::vector<int> done;
    std::atomic<int> drains{0};
    bool gateOpen = true; ///< Closed: the drain waits before its first item
    bool inDrain = false;

    void drain(const std::stop_token& stop)
    {
        ++drains;
        {
            std::unique_lock lock(mutex);
            inDrain = true;
            changed.notify_all();
            changed.wait(lock, [this] { return gateOpen; });
        }
        while (!stop.stop_requested())
        {
            int item = 0;
            {
                const std::scoped_lock lock(mutex);
                if (queue.empty())
                {
                    inDrain = false;
                    changed.notify_all();
                    return;
                }
                item = queue.front();
                queue.pop_front();
            }
            const std::scoped_lock lock(mutex);
            done.push_back(item);
            changed.notify_all();
        }
    }

    SubmitResult add(DrainTrigger& trigger, int item)
    {
        const std::scoped_lock lock(mutex);
        queue.push_back(item);
        const SubmitResult result = trigger.wake();
        if (result == SubmitResult::Refused)
        {
            queue.pop_back(); // the rule's step 3
        }
        return result;
    }

    bool waitFor(std::size_t count)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, DRAIN_WAIT, [&] { return done.size() >= count && !inDrain; });
    }
};

TEST(DrainTriggerTest, EveryItemIsDrainedInOrder)
{
    Owner owner;
    DrainTrigger trigger("ts-test-drain", [&owner](const std::stop_token& stop) { owner.drain(stop); });
    for (int i = 0; i < 20; ++i)
    {
        const SubmitResult result = owner.add(trigger, i);
        EXPECT_TRUE(result == SubmitResult::Queued || result == SubmitResult::Merged) << static_cast<int>(result);
    }
    ASSERT_TRUE(owner.waitFor(20));
    const std::scoped_lock lock(owner.mutex);
    EXPECT_EQ(owner.done.size(), 20U);
    for (int i = 0; i < 20; ++i)
    {
        EXPECT_EQ(owner.done[static_cast<std::size_t>(i)], i);
    }
}

TEST(DrainTriggerTest, AWakeWhileADrainWaitsMergesIntoIt)
{
    Owner owner;
    DrainTrigger trigger("ts-test-drain", [&owner](const std::stop_token& stop) { owner.drain(stop); });
    {
        const std::scoped_lock lock(owner.mutex);
        owner.gateOpen = false;
    }
    // The first drain runs and waits at the gate; the second is queued behind it; the third merges into the second.
    EXPECT_EQ(owner.add(trigger, 1), SubmitResult::Queued);
    {
        std::unique_lock lock(owner.mutex);
        ASSERT_TRUE(owner.changed.wait_for(lock, DRAIN_WAIT, [&] { return owner.inDrain; }));
    }
    EXPECT_EQ(owner.add(trigger, 2), SubmitResult::Queued);
    EXPECT_EQ(owner.add(trigger, 3), SubmitResult::Merged);
    {
        const std::scoped_lock lock(owner.mutex);
        owner.gateOpen = true;
    }
    owner.changed.notify_all();
    ASSERT_TRUE(owner.waitFor(3));
    EXPECT_LE(owner.drains.load(), 2) << "the merged wake ran no drain of its own";
}

TEST(DrainTriggerTest, AnItemQueuedAsADrainFindsTheQueueEmptyStillRuns)
{
    // The ordering the rule exists for: the drain that just found the queue empty has returned (or is returning),
    // and the wake() after the item is queued starts the next one, so the item never waits for good.
    Owner owner;
    DrainTrigger trigger("ts-test-drain", [&owner](const std::stop_token& stop) { owner.drain(stop); });
    for (int round = 0; round < 200; ++round)
    {
        static_cast<void>(owner.add(trigger, round));
        if (round % 7 == 0)
        {
            std::this_thread::yield();
        }
    }
    ASSERT_TRUE(owner.waitFor(200));
    const std::scoped_lock lock(owner.mutex);
    EXPECT_TRUE(owner.queue.empty());
    EXPECT_EQ(owner.done.size(), 200U);
}

TEST(DrainTriggerTest, ADrainThatThrowsFailsTheWorkerAndLaterWakesAreClosed)
{
    std::atomic<bool> ran{false};
    DrainTrigger trigger("ts-test-drain",
                         [&ran](const std::stop_token& /*stop*/)
                         {
                             ran = true;
                             throw std::runtime_error("drain broke");
                         });
    EXPECT_EQ(trigger.wake(), SubmitResult::Queued);
    const auto giveUp = std::chrono::steady_clock::now() + DRAIN_WAIT;
    while (!trigger.failed() && std::chrono::steady_clock::now() < giveUp)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(trigger.failed());
    EXPECT_TRUE(ran.load());
    EXPECT_NE(trigger.failure().find("drain broke"), std::string::npos) << trigger.failure();
    EXPECT_EQ(trigger.wake(), SubmitResult::Closed);
}

TEST(DrainTriggerTest, TheDestructorStopsADrainAtItsNextCheck)
{
    Owner owner;
    std::atomic<bool> stopped{false};
    {
        DrainTrigger trigger("ts-test-drain",
                             [&](const std::stop_token& stop)
                             {
                                 {
                                     const std::scoped_lock lock(owner.mutex);
                                     owner.inDrain = true;
                                     owner.changed.notify_all();
                                 }
                                 while (!stop.stop_requested())
                                 {
                                     std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                 }
                                 stopped = true;
                             });
        EXPECT_EQ(trigger.wake(), SubmitResult::Queued);
        std::unique_lock lock(owner.mutex);
        ASSERT_TRUE(owner.changed.wait_for(lock, DRAIN_WAIT, [&] { return owner.inDrain; }));
    }
    EXPECT_TRUE(stopped.load()) << "joined after the drain saw its stop";
}

TEST(DrainTriggerTest, NoThreadRunsBeforeTheFirstWake)
{
    std::atomic<int> drains{0};
    {
        const DrainTrigger trigger("ts-test-drain", [&drains](const std::stop_token& /*stop*/) { ++drains; });
        EXPECT_FALSE(trigger.failed());
        EXPECT_TRUE(trigger.failure().empty());
    }
    EXPECT_EQ(drains.load(), 0);
}

} // namespace
} // namespace Domain
