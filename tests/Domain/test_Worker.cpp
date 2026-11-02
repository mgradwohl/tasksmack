/// @file test_Worker.cpp
/// @brief Domain::Worker (#3343): a body that throws fails the worker rather than the process, a full queue
/// does what its Admission policy says and submit() reports it, cancellation reaches the job in flight and
/// its hook, shutdown drops queued work unrun, and publications arrive in order.

#include "AllocationFailureHook.h"
#include "Domain/Worker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace Domain
{
namespace
{

constexpr std::chrono::seconds WAIT{5}; // generous: only a broken worker gets near it

/// A job: an id and a key, and a token whose use count tells whether the job still exists.
struct TestJob
{
    int id = 0;
    int key = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

using TestWorker = Worker<TestJob, double>;

/// A gate the body waits at until the test opens it, or the job's stop token is signalled.
class Gate
{
  public:
    void open()
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_Open = true;
        }
        m_Changed.notify_all();
    }

    /// Waits for open() or @p stop. @return Whether the gate was opened.
    bool pass(const std::stop_token& stop)
    {
        std::unique_lock lock(m_Mutex);
        static_cast<void>(m_Changed.wait(lock, stop, [this] { return m_Open; }));
        return m_Open;
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable_any m_Changed;
    bool m_Open = false;
};

/// Records the ids of the jobs a body ran, in order.
class Ran
{
  public:
    void add(int id)
    {
        const std::scoped_lock lock(m_Mutex);
        m_Ids.push_back(id);
    }

    [[nodiscard]] std::vector<int> ids() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Ids;
    }

  private:
    mutable std::mutex m_Mutex;
    std::vector<int> m_Ids;
};

/// Waits until @p worker has a job in flight.
bool waitBusy(const TestWorker& worker)
{
    const auto deadline = std::chrono::steady_clock::now() + WAIT;
    while (!worker.busy())
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

TestWorker::Options options(std::size_t capacity, Admission admission)
{
    return {.threadName = "ts-test-worker", .capacity = capacity, .admission = admission};
}

TEST(WorkerTest, RunsJobsInOrderAndPublishesTheirResults)
{
    Ran ran;
    TestWorker worker(options(8, Admission::Refuse),
                      [&ran](TestJob& job, const std::stop_token& /*stop*/, TestWorker::JobContext& out)
                      {
                          ran.add(job.id);
                          out.finish(job.id * 0.5);
                      });
    EXPECT_EQ(worker.latest()->version, 0U);
    for (int id = 1; id <= 3; ++id)
    {
        EXPECT_EQ(worker.submit({.id = id}), SubmitResult::Queued);
    }
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(ran.ids(), (std::vector<int>{1, 2, 3}));
    const auto published = worker.latest();
    EXPECT_EQ(published->version, 3U);
    EXPECT_EQ(worker.publishedVersion(), 3U);
    EXPECT_TRUE(published->isFinal);
    EXPECT_DOUBLE_EQ(published->value, 1.5);
    EXPECT_EQ(worker.state(), WorkerState::Running);
    EXPECT_TRUE(worker.failure().empty());
}

TEST(WorkerTest, ABodyThatThrowsFailsTheWorkerWithItsReasonAndTheProcessGoesOn)
{
    Gate gate;
    TestWorker worker(options(8, Admission::Refuse),
                      [&gate](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& out)
                      {
                          static_cast<void>(gate.pass(stop));
                          out.progress(0.25);
                          if (job.id == 1)
                          {
                              throw std::runtime_error("the reader broke");
                          }
                      });
    const TestJob queued{.id = 2};
    const std::weak_ptr<int> queuedAlive = queued.alive;
    EXPECT_EQ(worker.submit({.id = 1}), SubmitResult::Queued);
    EXPECT_EQ(worker.submit(queued), SubmitResult::Queued);
    gate.open();
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));

    EXPECT_EQ(worker.state(), WorkerState::Failed);
    EXPECT_EQ(worker.failure(), "the reader broke");
    EXPECT_FALSE(worker.busy());
    EXPECT_EQ(worker.queued(), 0U); // the job queued behind it is dropped unrun
    EXPECT_DOUBLE_EQ(worker.latest()->value, 0.25);
    EXPECT_FALSE(worker.latest()->isFinal);
    // Failed is terminal: no more work is taken.
    EXPECT_EQ(worker.submit({.id = 3}), SubmitResult::Closed);
    EXPECT_EQ(queued.alive.use_count(), 1); // only the test's copy is left
    EXPECT_FALSE(queuedAlive.expired());
}

TEST(WorkerTest, ABodyThatThrowsANonStandardExceptionOrBadAllocFailsTheWorker)
{
    {
        TestWorker worker(options(1, Admission::Refuse),
                          [](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& /*out*/)
                          {
                              throw 42; // NOLINT(bugprone-std-exception-baseclass) - a non-std exception is the case under test
                          });
        EXPECT_EQ(worker.submit({}), SubmitResult::Queued);
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
        EXPECT_EQ(worker.state(), WorkerState::Failed);
        EXPECT_EQ(worker.failure(), TestWorker::FAILURE_UNKNOWN);
    }
    {
        TestWorker worker(options(1, Admission::Refuse),
                          [](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& /*out*/)
                          { throw std::bad_alloc(); });
        EXPECT_EQ(worker.submit({}), SubmitResult::Queued);
        ASSERT_TRUE(worker.waitUntilIdle(WAIT));
        EXPECT_EQ(worker.state(), WorkerState::Failed);
        EXPECT_FALSE(worker.failure().empty());
    }
}

TEST(WorkerTest, AnEmptyBodyFailsTheWorkerRatherThanTerminating)
{
    TestWorker worker(options(1, Admission::Refuse), TestWorker::Body{});
    EXPECT_EQ(worker.submit({}), SubmitResult::Queued);
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(worker.state(), WorkerState::Failed); // std::bad_function_call, contained
}

/// A worker whose body holds job 0 at a gate, so the test fills the queue behind it.
class FullQueueTest : public ::testing::Test
{
  protected:
    std::unique_ptr<TestWorker> make(Admission admission)
    {
        auto worker = std::make_unique<TestWorker>(
            TestWorker::Options{
                .threadName = "ts-test-worker",
                .capacity = 2,
                .admission = admission,
                .sameKey = [](const TestJob& queued, const TestJob& incoming) { return queued.key == incoming.key; },
            },
            [this](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& /*out*/)
            {
                if (job.id == 0)
                {
                    static_cast<void>(m_Gate.pass(stop));
                }
                m_Ran.add(job.id);
            });
        EXPECT_EQ(worker->submit({.id = 0, .key = 0}), SubmitResult::Queued);
        EXPECT_TRUE(waitBusy(*worker));
        return worker;
    }

    Gate m_Gate;
    Ran m_Ran;
};

TEST_F(FullQueueTest, RefuseKeepsTheQueueAndRefusesTheNewJob)
{
    const auto worker = make(Admission::Refuse);
    EXPECT_EQ(worker->submit({.id = 1, .key = 1}), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 2, .key = 2}), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 3, .key = 1}), SubmitResult::Refused); // same key, but Refuse doesn't merge
    EXPECT_EQ(worker->queued(), 2U);
    m_Gate.open();
    ASSERT_TRUE(worker->waitUntilIdle(WAIT));
    EXPECT_EQ(m_Ran.ids(), (std::vector<int>{0, 1, 2}));
    // Room again: admitted.
    EXPECT_EQ(worker->submit({.id = 4, .key = 4}), SubmitResult::Queued);
}

TEST_F(FullQueueTest, DropOldestDropsTheOldestQueuedJobUnrun)
{
    const auto worker = make(Admission::DropOldest);
    const TestJob first{.id = 1, .key = 1};
    EXPECT_EQ(worker->submit(first), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 2, .key = 2}), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 3, .key = 3}), SubmitResult::DroppedOldest);
    EXPECT_EQ(worker->queued(), 2U);
    EXPECT_EQ(first.alive.use_count(), 1); // the dropped job is gone already, not kept for later
    m_Gate.open();
    ASSERT_TRUE(worker->waitUntilIdle(WAIT));
    EXPECT_EQ(m_Ran.ids(), (std::vector<int>{0, 2, 3}));
}

TEST_F(FullQueueTest, MergeByKeyReplacesTheQueuedJobInItsTurnAndRefusesANewKeyWhenFull)
{
    const auto worker = make(Admission::MergeByKey);
    EXPECT_EQ(worker->submit({.id = 1, .key = 1}), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 2, .key = 2}), SubmitResult::Queued);
    EXPECT_EQ(worker->submit({.id = 11, .key = 1}), SubmitResult::Merged);
    EXPECT_EQ(worker->submit({.id = 3, .key = 3}), SubmitResult::Refused);
    EXPECT_EQ(worker->submit({.id = 0, .key = 0}), SubmitResult::Refused); // the job in flight isn't merged with
    EXPECT_EQ(worker->queued(), 2U);
    m_Gate.open();
    ASSERT_TRUE(worker->waitUntilIdle(WAIT));
    EXPECT_EQ(m_Ran.ids(), (std::vector<int>{0, 11, 2}));
}

TEST(WorkerTest, MergeByKeyMergesBeforeTheQueueIsFull)
{
    Gate gate;
    Ran ran;
    TestWorker worker(
        {
            .threadName = "ts-test-worker",
            .capacity = 4,
            .admission = Admission::MergeByKey,
            .sameKey = [](const TestJob& queued, const TestJob& incoming) { return queued.key == incoming.key; },
        },
        [&gate, &ran](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& /*out*/)
        {
            if (job.id == 0)
            {
                static_cast<void>(gate.pass(stop));
            }
            ran.add(job.id);
        });
    EXPECT_EQ(worker.submit({.id = 0, .key = 0}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    EXPECT_EQ(worker.submit({.id = 1, .key = 7}), SubmitResult::Queued);
    EXPECT_EQ(worker.submit({.id = 2, .key = 7}), SubmitResult::Merged);
    EXPECT_EQ(worker.queued(), 1U);
    gate.open();
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(ran.ids(), (std::vector<int>{0, 2}));
}

// Admission is transactional because a Job's moves can't throw: Worker static_asserts both, so a Job type whose
// move could throw (and leave a merged job half-assigned, or the queue over its bound) doesn't compile.
static_assert(std::is_nothrow_move_constructible_v<TestJob> && std::is_nothrow_move_assignable_v<TestJob>);

TEST(WorkerTest, ASameKeyThatThrowsRefusesAndLeavesTheQueueAsItWas)
{
    Gate gate;
    Ran ran;
    TestWorker worker(
        {
            .threadName = "ts-test-worker",
            .capacity = 4,
            .admission = Admission::MergeByKey,
            .sameKey =
                [](const TestJob& queued, const TestJob& incoming)
            {
                if (incoming.key == 99)
                {
                    throw std::runtime_error("no key");
                }
                return queued.key == incoming.key;
            },
        },
        [&gate, &ran](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& /*context*/)
        {
            if (job.id == 0)
            {
                static_cast<void>(gate.pass(stop));
            }
            ran.add(job.id);
        });
    EXPECT_EQ(worker.submit({.id = 0, .key = 0}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    EXPECT_EQ(worker.submit({.id = 1, .key = 1}), SubmitResult::Queued);
    EXPECT_EQ(worker.submit({.id = 2, .key = 99}), SubmitResult::Refused);
    EXPECT_EQ(worker.queued(), 1U);
    gate.open();
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(ran.ids(), (std::vector<int>{0, 1}));
    EXPECT_EQ(worker.state(), WorkerState::Running);
}

// Shutdown allocates nothing, so it can't terminate the process when memory has run out: stop() with a job
// in flight (and its hook armed) and jobs queued, and the destructor of a worker that failed, both run with
// the next allocation on their thread made to fail. The hook is compiled out on Windows and in sanitizer
// builds (AllocationFailureHook.h), where these skip; the Linux debug and release builds run them.
TEST(WorkerTest, StopAllocatesNothingWithAJobInFlightAndJobsQueued)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    std::atomic<int> hookCalls{0};
    TestWorker worker(options(4, Admission::Refuse),
                      [&hookCalls](TestJob& /*job*/, const std::stop_token& stop, TestWorker::JobContext& context)
                      {
                          auto hook = [&hookCalls] noexcept
                          {
                              hookCalls.fetch_add(1);
                          };
                          const auto armed = context.armCancel(hook);
                          Gate never;
                          static_cast<void>(never.pass(stop));
                      });
    EXPECT_EQ(worker.submit({.id = 1}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    EXPECT_EQ(worker.submit({.id = 2}), SubmitResult::Queued);
    EXPECT_EQ(worker.submit({.id = 3}), SubmitResult::Queued);
    std::size_t dropped = 0;
    bool stillPending = false;
    {
        const TestSupport::FailAllocationAfter failNext(0);
        dropped = worker.stop();
        stillPending = TestSupport::allocationFailurePending();
    }
    EXPECT_TRUE(stillPending); // stop() reached no allocation
    EXPECT_EQ(dropped, 2U);
    EXPECT_EQ(hookCalls.load(), 1);
    EXPECT_EQ(worker.state(), WorkerState::Stopped);
#endif
}

TEST(WorkerTest, TheDestructorOfAFailedWorkerAllocatesNothing)
{
#ifdef TASKSMACK_NO_ALLOCATOR_HOOK
    GTEST_SKIP() << TASKSMACK_NO_ALLOCATOR_HOOK;
#else
    auto worker = std::make_unique<TestWorker>(options(1, Admission::Refuse),
                                               [](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& /*context*/)
                                               { throw std::runtime_error("the reader broke"); });
    EXPECT_EQ(worker->submit({}), SubmitResult::Queued);
    ASSERT_TRUE(worker->waitUntilIdle(WAIT));
    ASSERT_EQ(worker->state(), WorkerState::Failed);
    bool stillPending = false;
    {
        const TestSupport::FailAllocationAfter failNext(0);
        worker.reset();
        stillPending = TestSupport::allocationFailurePending();
    }
    EXPECT_TRUE(stillPending);
#endif
}

TEST(WorkerTest, CancelCurrentStopsTheJobInFlightCallsItsArmedHookOnceAndTheQueueGoesOn)
{
    std::atomic<int> hookCalls{0};
    std::atomic<bool> sawStop{false};
    Ran ran;
    TestWorker worker(options(4, Admission::Refuse),
                      [&hookCalls, &sawStop, &ran](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& context)
                      {
                          if (job.id == 1)
                          {
                              auto hook = [&hookCalls] noexcept
                              {
                                  hookCalls.fetch_add(1);
                              };
                              const auto armed = context.armCancel(hook);
                              Gate never;
                              static_cast<void>(never.pass(stop)); // only a stop ends this
                              sawStop = stop.stop_requested();
                          }
                          ran.add(job.id);
                          context.finish(static_cast<double>(job.id));
                      });
    worker.cancelCurrent(); // idle: nothing to cancel, so no hook
    EXPECT_EQ(hookCalls.load(), 0);

    EXPECT_EQ(worker.submit({.id = 1}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    EXPECT_EQ(worker.submit({.id = 2}), SubmitResult::Queued);
    worker.cancelCurrent();
    worker.cancelCurrent(); // a second cancel of the same arming doesn't call the hook again
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));

    EXPECT_TRUE(sawStop.load());
    EXPECT_EQ(hookCalls.load(), 1);
    EXPECT_EQ(ran.ids(), (std::vector<int>{1, 2}));
    EXPECT_EQ(worker.state(), WorkerState::Running);
    EXPECT_DOUBLE_EQ(worker.latest()->value, 2.0);

    worker.cancelCurrent(); // idle again, and job 2 armed nothing
    EXPECT_EQ(hookCalls.load(), 1);
}

TEST(WorkerTest, AHookArmedAfterTheCancelIsCalledAtOnceAndGuardsNest)
{
    // Which hook was called first and second (0: none); the worker thread's, read after it is idle.
    std::array<int, 2> calls{};
    std::size_t callCount = 0;
    Gate cancelled;
    TestWorker worker(options(1, Admission::Refuse),
                      [&calls, &callCount, &cancelled](TestJob& /*job*/, const std::stop_token& stop, TestWorker::JobContext& context)
                      {
                          static_cast<void>(cancelled.pass(stop)); // returns at the cancel
                          auto outer = [&calls, &callCount] noexcept
                          {
                              if (callCount < calls.size())
                              {
                                  calls[callCount++] = 1;
                              }
                          };
                          const auto outerArmed = context.armCancel(outer); // cancelled already: called here
                          {
                              auto inner = [&calls, &callCount] noexcept
                              {
                                  if (callCount < calls.size())
                                  {
                                      calls[callCount++] = 2;
                                  }
                              };
                              const auto innerArmed = context.armCancel(inner); // also at once
                          }
                      });
    EXPECT_EQ(worker.submit({}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    worker.cancelCurrent(); // nothing armed yet
    ASSERT_TRUE(worker.waitUntilIdle(WAIT));
    EXPECT_EQ(callCount, 2U);
    EXPECT_EQ(calls, (std::array<int, 2>{1, 2}));
}

// The hook's target is body-local: it must outlive every call of the hook. A canceller hammers
// cancelCurrent() while jobs arm, finish and tear down; the hook must never see its target gone. Bounded:
// 2000 jobs or 1.5 s, whichever comes first.
TEST(WorkerTest, CancelRacingCompletionNeverCallsAHookAfterItsTargetIsGone)
{
    struct Target
    {
        std::atomic<bool> alive{true};
    };
    std::atomic<int> afterTeardown{0};
    std::atomic<int> finished{0};
    TestWorker worker(options(4, Admission::Refuse),
                      [&afterTeardown, &finished](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& context)
                      {
                          Target target;
                          {
                              auto hook = [&target, &afterTeardown] noexcept
                              {
                                  if (!target.alive.load(std::memory_order_relaxed))
                                  {
                                      afterTeardown.fetch_add(1, std::memory_order_relaxed);
                                  }
                              };
                              const auto armed = context.armCancel(hook);
                              std::this_thread::yield(); // some work under the hook
                          } // disarmed, waiting out a running call, before the target is torn down
                          target.alive.store(false, std::memory_order_relaxed);
                          finished.fetch_add(1, std::memory_order_relaxed);
                      });
    std::atomic<bool> done{false};
    std::jthread canceller(
        [&worker, &done]
        {
            while (!done.load())
            {
                worker.cancelCurrent();
            }
        });
    constexpr int JOBS = 2000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{1500};
    int submitted = 0;
    while (submitted < JOBS && std::chrono::steady_clock::now() < deadline)
    {
        if (worker.submit({.id = submitted}) == SubmitResult::Queued)
        {
            ++submitted;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    EXPECT_TRUE(worker.waitUntilIdle(WAIT));
    done = true;
    canceller.join();
    EXPECT_EQ(afterTeardown.load(), 0);
    EXPECT_EQ(finished.load(), submitted);
    EXPECT_EQ(worker.state(), WorkerState::Running);
}

TEST(WorkerTest, StopCancelsTheJobInFlightAndDropsTheQueueUnrun)
{
    std::atomic<int> hookCalls{0};
    Ran ran;
    std::vector<TestJob> jobs(3);
    for (std::size_t i = 0; i < jobs.size(); ++i)
    {
        jobs[i].id = static_cast<int>(i) + 1;
    }
    auto worker =
        std::make_unique<TestWorker>(options(4, Admission::Refuse),
                                     [&hookCalls, &ran](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& context)
                                     {
                                         auto hook = [&hookCalls] noexcept
                                         {
                                             hookCalls.fetch_add(1);
                                         };
                                         const auto armed = context.armCancel(hook);
                                         Gate never;
                                         static_cast<void>(never.pass(stop));
                                         ran.add(job.id);
                                     });
    for (const TestJob& job : jobs)
    {
        EXPECT_EQ(worker->submit(job), SubmitResult::Queued);
    }
    ASSERT_TRUE(waitBusy(*worker));
    EXPECT_EQ(worker->stop(), 2U);
    EXPECT_EQ(worker->state(), WorkerState::Stopped);
    EXPECT_EQ(hookCalls.load(), 1);
    EXPECT_EQ(ran.ids(), (std::vector<int>{1})); // the job in flight ended at its stop; the rest never ran
    for (const TestJob& job : jobs)
    {
        EXPECT_EQ(job.alive.use_count(), 1); // every queued copy is destroyed
    }
    EXPECT_EQ(worker->submit({.id = 9}), SubmitResult::Closed);
    EXPECT_EQ(worker->stop(), 0U); // again: nothing left
    worker.reset();                // the destructor after stop(): nothing to wait for
    EXPECT_EQ(hookCalls.load(), 1);
}

TEST(WorkerTest, TheDestructorEndsAWorkerWithQueuedWork)
{
    Ran ran;
    const TestJob queued{.id = 2};
    {
        TestWorker worker(options(4, Admission::Refuse),
                          [&ran](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& /*out*/)
                          {
                              Gate never;
                              static_cast<void>(never.pass(stop));
                              ran.add(job.id);
                          });
        EXPECT_EQ(worker.submit({.id = 1}), SubmitResult::Queued);
        EXPECT_EQ(worker.submit(queued), SubmitResult::Queued);
        ASSERT_TRUE(waitBusy(worker));
    }
    EXPECT_EQ(ran.ids(), (std::vector<int>{1}));
    EXPECT_EQ(queued.alive.use_count(), 1);
}

TEST(WorkerTest, AWorkerNeverSubmittedToStartsNoThreadAndStopsCleanly)
{
    TestWorker worker(options(1, Admission::Refuse),
                      [](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& /*out*/) {});
    EXPECT_FALSE(worker.busy());
    EXPECT_TRUE(worker.waitUntilIdle(std::chrono::milliseconds{0}));
    EXPECT_EQ(worker.stop(), 0U);
    EXPECT_EQ(worker.state(), WorkerState::Stopped);
}

TEST(WorkerTest, IncrementalPublicationsAreObservedInOrderThenTheFinalOne)
{
    constexpr int STEPS = 5;
    std::binary_semaphore published{0};
    std::binary_semaphore seen{0};
    TestWorker worker(options(1, Admission::Refuse),
                      [&published, &seen](TestJob& /*job*/, const std::stop_token& /*stop*/, TestWorker::JobContext& out)
                      {
                          for (int step = 1; step <= STEPS; ++step)
                          {
                              out.progress(static_cast<double>(step) / STEPS);
                              published.release();
                              if (!seen.try_acquire_for(WAIT)) // the test reads each one before the next
                              {
                                  return; // it gave up (a failed assertion): end, so the worker can be joined
                              }
                          }
                          out.finish(1.0);
                          published.release();
                      });
    EXPECT_EQ(worker.submit({}), SubmitResult::Queued);
    for (int step = 1; step <= STEPS; ++step)
    {
        ASSERT_TRUE(published.try_acquire_for(WAIT));
        const auto progress = worker.latest();
        EXPECT_EQ(progress->version, static_cast<std::uint64_t>(step));
        EXPECT_FALSE(progress->isFinal);
        EXPECT_DOUBLE_EQ(progress->value, static_cast<double>(step) / STEPS);
        seen.release();
    }
    ASSERT_TRUE(published.try_acquire_for(WAIT));
    const auto result = worker.latest();
    EXPECT_EQ(result->version, static_cast<std::uint64_t>(STEPS) + 1);
    EXPECT_TRUE(result->isFinal);
    EXPECT_DOUBLE_EQ(result->value, 1.0);
}

TEST(WorkerTest, ACapacityOfZeroHoldsOneJob)
{
    Gate gate;
    TestWorker worker(options(0, Admission::Refuse),
                      [&gate](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& /*out*/)
                      {
                          if (job.id == 0)
                          {
                              static_cast<void>(gate.pass(stop));
                          }
                      });
    EXPECT_EQ(worker.submit({.id = 0}), SubmitResult::Queued);
    ASSERT_TRUE(waitBusy(worker));
    EXPECT_EQ(worker.submit({.id = 1}), SubmitResult::Queued);
    EXPECT_EQ(worker.submit({.id = 2}), SubmitResult::Refused);
    gate.open();
    EXPECT_TRUE(worker.waitUntilIdle(WAIT));
}

// Submitters, a canceller and a reader race one worker for a short, bounded time: the queue never exceeds
// its bound, every admitted job is either run or dropped (none lost, none twice), and publications never go
// backwards. ThreadSanitizer builds check the locking.
TEST(WorkerTest, StressSubmitCancelAndReadStayConsistent)
{
    constexpr std::size_t CAPACITY = 8;
    const unsigned cores = std::max(2U, std::thread::hardware_concurrency());
    // At most 1.5x the cores in all: submitters plus the canceller and the reader (and the worker itself).
    const unsigned submitters = std::max(1U, std::min(4U, ((cores * 3U) / 2U) - 3U));

    std::atomic<std::uint64_t> ranCount{0};
    std::atomic<bool> overBound{false};
    TestWorker worker(
        {
            .threadName = "ts-test-worker",
            .capacity = CAPACITY,
            .admission = Admission::DropOldest,
        },
        [&ranCount](TestJob& job, const std::stop_token& stop, TestWorker::JobContext& out)
        {
            out.progress(static_cast<double>(job.id));
            if (!stop.stop_requested())
            {
                out.finish(static_cast<double>(job.id));
            }
            ranCount.fetch_add(1, std::memory_order_relaxed);
        });

    std::atomic<std::uint64_t> admitted{0};
    std::atomic<std::uint64_t> droppedOldest{0};
    std::atomic<bool> done{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{300};
    {
        std::vector<std::jthread> threads;
        threads.reserve(submitters + 2);
        for (unsigned t = 0; t < submitters; ++t)
        {
            threads.emplace_back(
                [&, t]
                {
                    int id = static_cast<int>(t) * 1000000;
                    while (std::chrono::steady_clock::now() < deadline)
                    {
                        const SubmitResult result = worker.submit({.id = ++id});
                        if (result == SubmitResult::Queued || result == SubmitResult::DroppedOldest)
                        {
                            admitted.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (result == SubmitResult::DroppedOldest)
                        {
                            droppedOldest.fetch_add(1, std::memory_order_relaxed);
                        }
                        EXPECT_NE(result, SubmitResult::Closed);
                    }
                });
        }
        threads.emplace_back(
            [&]
            {
                unsigned turn = 0;
                while (!done.load())
                {
                    if (++turn % 4 == 0) // a cancel every fourth turn
                    {
                        worker.cancelCurrent();
                    }
                    std::this_thread::yield();
                }
            });
        threads.emplace_back(
            [&]
            {
                std::uint64_t last = 0;
                while (!done.load())
                {
                    const auto published = worker.latest();
                    EXPECT_GE(published->version, last);
                    last = published->version;
                    if (worker.queued() > CAPACITY)
                    {
                        overBound = true;
                    }
                }
            });
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        // The submitters end at the deadline; then the others.
        for (unsigned t = 0; t < submitters; ++t)
        {
            threads[t].join();
        }
        done = true;
    }
    const std::size_t droppedAtStop = worker.stop();
    EXPECT_FALSE(overBound.load());
    EXPECT_EQ(worker.state(), WorkerState::Stopped);
    EXPECT_EQ(ranCount.load() + droppedOldest.load() + droppedAtStop, admitted.load());
    EXPECT_GT(ranCount.load(), 0U);
}

} // namespace
} // namespace Domain
