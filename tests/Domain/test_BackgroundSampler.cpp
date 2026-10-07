/// @file test_BackgroundSampler.cpp
/// @brief Comprehensive tests for Domain::BackgroundSampler
///
/// Tests cover:
/// - Start/stop lifecycle
/// - Callback invocation
/// - Interval configuration
/// - Refresh requests
/// - Thread safety
/// - Capabilities passthrough

#include "Domain/BackgroundSampler.h"
#include "Domain/ISamplable.h"
#include "Domain/SamplingConfig.h"
#include "Platform/ThreadName.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <pthread.h>
#endif

using namespace std::chrono_literals;

namespace
{

class MockSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        std::lock_guard lock(mtx);
        sampleCount++;
        cv.notify_all();
    }

    int getSampleCount() const
    {
        std::lock_guard lock(mtx);
        return sampleCount;
    }

    void waitForSamples(int targetCount)
    {
        std::unique_lock lock(mtx);
        const bool success = cv.wait_for(lock, 2000ms, [this, targetCount] { return sampleCount >= targetCount; });
        if (!success)
        {
            ADD_FAILURE() << "Timeout waiting for " << targetCount << " samples. Actual: " << sampleCount;
        }
    }

  private:
    mutable std::mutex mtx;
    std::condition_variable cv;
    int sampleCount = 0;
};

template<typename Predicate> [[nodiscard]] bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 2000ms)
{
    constexpr auto POLL_INTERVAL = 5ms;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(POLL_INTERVAL);
    }
    return true;
}

} // namespace

// =============================================================================
// Construction Tests
// =============================================================================

TEST(BackgroundSamplerTest, ConstructWithValidProbe)
{
    Domain::BackgroundSampler sampler;

    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, ConstructWithCustomInterval)
{
    Domain::SamplerConfig config;
    config.interval = 500ms;

    Domain::BackgroundSampler sampler(config);

    EXPECT_EQ(sampler.interval(), 500ms);
}

TEST(BackgroundSamplerTest, DefaultIntervalIsOneSecond)
{
    Domain::BackgroundSampler sampler;

    EXPECT_EQ(sampler.interval(), 1000ms);
}

// =============================================================================
// Start/Stop Lifecycle Tests
// =============================================================================

TEST(BackgroundSamplerTest, StartSetsRunningTrue)
{
    Domain::BackgroundSampler sampler;

    sampler.start();
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, StopWhenNotRunningIsNoOp)
{
    Domain::BackgroundSampler sampler;

    // Should not crash
    sampler.stop();
    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, DoubleStartIsIgnored)
{
    Domain::BackgroundSampler sampler;

    sampler.start();
    sampler.start(); // Should be ignored
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
}

TEST(BackgroundSamplerTest, DestructorStopsSampler)
{
    {
        Domain::BackgroundSampler sampler;
        sampler.start();
        EXPECT_TRUE(sampler.isRunning());
        // Destructor should stop the sampler
    }
    // If we get here without hanging, the test passes
    SUCCEED();
}

// =============================================================================
// ISamplable Tests
// =============================================================================

TEST(BackgroundSamplerTest, SampleInvokedOnInterval)
{
    auto samplable1 = std::make_shared<MockSamplable>();
    auto samplable2 = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 50ms; // Fast sampling for test

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable1);
    sampler.addSamplable(samplable2);

    sampler.start();

    // Wait for samples
    samplable1->waitForSamples(1);
    samplable2->waitForSamples(1);

    sampler.stop();

    EXPECT_GE(samplable1->getSampleCount(), 1);
    EXPECT_GE(samplable2->getSampleCount(), 1);
}

TEST(BackgroundSamplerTest, SampleInvokedMultipleTimes)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 30ms; // Fast sampling for test

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    sampler.start();

    // Wait for multiple callbacks
    samplable->waitForSamples(3);

    sampler.stop();

    // Should have been called multiple times
    EXPECT_GE(samplable->getSampleCount(), 3);
}

TEST(BackgroundSamplerTest, EmptySamplablesListDoesNotCrash)
{
    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(config);

    // Don't add any samplables
    sampler.start();
    std::this_thread::sleep_for(100ms);
    sampler.stop();

    // Should not crash
    SUCCEED();
}
// =============================================================================
// Interval Configuration Tests
// =============================================================================

TEST(BackgroundSamplerTest, SetIntervalWhileRunning)
{
    Domain::SamplerConfig config;
    config.interval = 500ms;

    Domain::BackgroundSampler sampler(config);

    sampler.start();
    EXPECT_EQ(sampler.interval(), 500ms);

    sampler.setInterval(100ms);
    EXPECT_EQ(sampler.interval(), 100ms);

    sampler.stop();
}

// #1118: interval changes rebase on the last sample, so changing it repeatedly can't postpone
// sampling indefinitely (the interaction throttle toggles it on every short drag).
TEST(BackgroundSamplerTest, RepeatedIntervalChangesDoNotStarveSampling)
{
    auto samplable = std::make_shared<MockSamplable>();
    Domain::SamplerConfig config;
    config.interval = 200ms;
    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);
    sampler.start();
    samplable->waitForSamples(1);
    const int afterFirst = samplable->getSampleCount();

    // Change the interval every 40 ms for 900 ms, never further than 300 ms out.
    const auto until = std::chrono::steady_clock::now() + 900ms;
    bool longer = false;
    while (std::chrono::steady_clock::now() < until)
    {
        sampler.setInterval(longer ? 300ms : 250ms);
        longer = !longer;
        std::this_thread::sleep_for(40ms);
    }
    const int sampled = samplable->getSampleCount() - afterFirst;
    sampler.stop();

    // At most ~300 ms apart, so about three samples; resetting the wait each time gave none.
    EXPECT_GE(sampled, 2);
}

// #1102: a sampler whose owner already took the seed sample waits an interval before sampling.
TEST(BackgroundSamplerTest, SeededSamplerWaitsAnIntervalBeforeItsFirstSample)
{
    auto samplable = std::make_shared<MockSamplable>();
    Domain::SamplerConfig config;
    config.interval = 300ms;
    config.firstSampleAfterInterval = true;
    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    const auto started = std::chrono::steady_clock::now();
    sampler.start();
    samplable->waitForSamples(1);
    const auto firstSampleAfter = std::chrono::steady_clock::now() - started;
    sampler.stop();

    // Not straight after the seed, but one interval later. A wait can only overrun, so the lower
    // bound is exact (less a little clock slack); the upper bound leaves room for a loaded runner
    // while still catching a sampler that waits several intervals.
    EXPECT_GE(firstSampleAfter, 290ms);
    EXPECT_LT(firstSampleAfter, 1000ms);
}

TEST(BackgroundSamplerTest, SetIntervalWhileStopped)
{
    Domain::BackgroundSampler sampler;

    EXPECT_EQ(sampler.interval(), 1000ms);
    sampler.setInterval(250ms);
    EXPECT_EQ(sampler.interval(), 250ms);
}

TEST(BackgroundSamplerTest, SetIntervalBeforeStartDoesNotScheduleDuplicateSample)
{
    auto samplable = std::make_shared<MockSamplable>();
    Domain::BackgroundSampler sampler;
    sampler.addSamplable(samplable);

    sampler.setInterval(5000ms);
    sampler.start();
    samplable->waitForSamples(1);

    EXPECT_FALSE(waitFor([&] { return samplable->getSampleCount() > 1; }, 100ms));

    sampler.stop();
    EXPECT_EQ(samplable->getSampleCount(), 1);
}

// =============================================================================
// Refresh Request Tests
// =============================================================================

TEST(BackgroundSamplerTest, RefreshRightAfterASampleWaitsForAUsableInterval)
{
    // #1102: a refresh forced milliseconds after a sample must not sample again at once -- the
    // models would get no usable deltas (0% CPU, false 0 B/s). It waits until at least the fastest
    // supported interval has passed since the previous sample.
    class TimedSamplable : public Domain::ISamplable
    {
      public:
        void sample() override
        {
            const std::scoped_lock lock(m_Mutex);
            m_Times.push_back(std::chrono::steady_clock::now());
            m_Cv.notify_all();
        }
        [[nodiscard]] std::vector<std::chrono::steady_clock::time_point> waitForSamples(std::size_t count)
        {
            std::unique_lock lock(m_Mutex);
            m_Cv.wait_for(lock, 2000ms, [&] { return m_Times.size() >= count; });
            return m_Times;
        }

      private:
        std::mutex m_Mutex;
        std::condition_variable m_Cv;
        std::vector<std::chrono::steady_clock::time_point> m_Times;
    };

    auto samplable = std::make_shared<TimedSamplable>();
    Domain::SamplerConfig config;
    config.interval = 1000ms;
    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);
    sampler.start();
    ASSERT_EQ(samplable->waitForSamples(1).size(), 1U);

    sampler.requestRefresh(); // immediately after the first sample
    const auto times = samplable->waitForSamples(2);
    sampler.stop();

    ASSERT_GE(times.size(), 2U);
    const auto gap = times[1] - times[0];
    EXPECT_GE(gap, std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS) - 5ms); // waited
    EXPECT_LT(gap, 900ms); // but still early: the refresh wasn't dropped
}

TEST(BackgroundSamplerTest, RequestRefreshTriggersEarlySample)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 10000ms; // Long interval

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    sampler.start();

    // Wait for first sample
    samplable->waitForSamples(1);
    int countAfterFirst = samplable->getSampleCount();

    // Request refresh - should trigger another sample quickly
    sampler.requestRefresh();
    samplable->waitForSamples(countAfterFirst + 1);

    sampler.stop();

    // Should have gotten at least one more sample after refresh request
    EXPECT_GT(samplable->getSampleCount(), countAfterFirst);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(BackgroundSamplerTest, ConcurrentIntervalChanges)
{
    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(config);
    sampler.start();

    // Multiple threads changing interval concurrently
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i)
    {
        threads.emplace_back(
            [&sampler, i]()
            {
                for (int j = 0; j < 20; ++j)
                {
                    sampler.setInterval(std::chrono::milliseconds(50 + (i * 10) + j));
                    std::this_thread::sleep_for(5ms);
                }
            });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    sampler.stop();

    // Should not crash, interval should be some valid value
    EXPECT_GT(sampler.interval().count(), 0);
}

TEST(BackgroundSamplerTest, ConcurrentSamplableAdd)
{
    Domain::SamplerConfig config;
    config.interval = 30ms;

    Domain::BackgroundSampler sampler(config);

    std::vector<std::shared_ptr<MockSamplable>> samplables;
    samplables.reserve(10);
    for (int i = 0; i < 10; ++i)
    {
        samplables.push_back(std::make_shared<MockSamplable>());
    }

    sampler.start();

    // Change samplables while sampler is running
    for (const auto& samplable : samplables)
    {
        sampler.addSamplable(samplable);
        std::this_thread::sleep_for(10ms);
    }

    sampler.stop();

    // Should not crash
    SUCCEED();
}

TEST(BackgroundSamplerTest, ConcurrentRefreshRequests)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 200ms;

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    sampler.start();

    // Multiple threads requesting refresh
    std::vector<std::thread> threads;
    for (int i = 0; i < 5; ++i)
    {
        threads.emplace_back(
            [&sampler]()
            {
                for (int j = 0; j < 10; ++j)
                {
                    sampler.requestRefresh();
                    std::this_thread::sleep_for(10ms);
                }
            });
    }

    for (auto& t : threads)
    {
        t.join();
    }

    sampler.stop();

    EXPECT_GT(samplable->getSampleCount(), 1);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(BackgroundSamplerTest, VeryShortIntervalIsClamped)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 1ms; // Very short

    Domain::BackgroundSampler sampler(config);
    EXPECT_EQ(sampler.interval(), std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));
    sampler.addSamplable(samplable);

    sampler.start();
    samplable->waitForSamples(1);
    sampler.stop();

    EXPECT_GE(samplable->getSampleCount(), 1);
}

TEST(BackgroundSamplerTest, StartStopStartCycle)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    // Start/stop cycle multiple times
    for (int i = 0; i < 3; ++i)
    {
        // Wait for a sample from this run -- counted from before start(), since the previous run
        // may have sampled more than once before it stopped -- rather than sleeping a fixed time,
        // which a loaded sanitizer runner can outlast (#1136).
        const int samplesBefore = samplable->getSampleCount();
        sampler.start();
        EXPECT_TRUE(sampler.isRunning());
        samplable->waitForSamples(samplesBefore + 1);
        sampler.stop();
        EXPECT_FALSE(sampler.isRunning());
    }

    EXPECT_GE(samplable->getSampleCount(), 3);
}

TEST(BackgroundSamplerTest, ZeroIntervalIsClamped)
{
    // Edge case: zero interval
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 0ms; // Zero interval - sleepTime will always be <= 0

    Domain::BackgroundSampler sampler(config);
    EXPECT_EQ(sampler.interval(), std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));
    sampler.addSamplable(samplable);

    sampler.start();
    // The sampler loop samples immediately on its first iteration, before waiting out the
    // (clamped) interval, but a fixed 50ms sleep was racy against thread-startup/scheduling
    // latency on a loaded CI runner -- poll instead, like the other tests in this file.
    // waitFor()'s own return value isn't asserted on: it could return false right at the
    // timeout boundary even if the sample lands immediately after the last predicate check
    // (or while stop() is joining), which would be a false-negative test failure despite
    // getSampleCount() being correct by the time stop() returns. It's still used to avoid an
    // unconditional worst-case wait, just not as the pass/fail signal.
    (void) waitFor([&] { return samplable->getSampleCount() >= 1; });
    sampler.stop();

    EXPECT_GE(samplable->getSampleCount(), 1);
}

// =============================================================================
// Exception Safety Tests
// =============================================================================

class ThrowingSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        m_CallCount++;
        throw std::runtime_error("simulated sample failure");
    }

    int getCallCount() const
    {
        return m_CallCount;
    }

  private:
    std::atomic<int> m_CallCount{0};
};

TEST(BackgroundSamplerTest, SamplableThrowingSamplerContinues)
{
    auto samplable = std::make_shared<ThrowingSamplable>();

    Domain::SamplerConfig config;
    config.interval = 10ms; // Fast interval to accumulate multiple exceptions quickly

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);
    sampler.start();

    const bool continuedAfterException = waitFor([&] { return samplable->getCallCount() > 1; });

    // Sampler should still be running despite repeated exceptions
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();

    EXPECT_TRUE(continuedAfterException);
}

class ThrowingNonStdSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        m_CallCount++;
        // NOLINTNEXTLINE(hicpp-exception-baseclass) - intentionally non-std for coverage
        throw 42;
    }

    int getCallCount() const
    {
        return m_CallCount;
    }

  private:
    std::atomic<int> m_CallCount{0};
};

TEST(BackgroundSamplerTest, SamplableThrowingNonStdExceptionSamplerContinues)
{
    auto samplable = std::make_shared<ThrowingNonStdSamplable>();

    Domain::SamplerConfig config;
    config.interval = 10ms;

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);

    sampler.start();

    const bool continuedAfterException = waitFor([&] { return samplable->getCallCount() >= 2; });

    EXPECT_TRUE(continuedAfterException);
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
}

// =============================================================================
// Destruction Safety Tests
// =============================================================================

// Regression test for a destruction-order hazard: BackgroundSampler used to store samplables
// as raw ISamplable* with no ownership tracking, so an owner destroying its samplable while the
// sampler thread was still running (or ever adding a future samplable without also updating
// every owning panel's destructor to stop the sampler first) would risk a use-after-free. Now
// that addSamplable() takes a weak_ptr, this is safe by construction: locking an expired
// weak_ptr just returns nullptr and the sampler quietly skips that iteration.
TEST(BackgroundSamplerTest, SamplableDestroyedWhileSamplerRunningDoesNotCrash)
{
    auto samplable = std::make_shared<MockSamplable>();

    Domain::SamplerConfig config;
    config.interval = 5ms; // Fast sampling to maximize the chance of racing the reset() below

    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(samplable);
    sampler.start();

    samplable->waitForSamples(1);

    // Destroy the samplable (simulating a panel destroying its model) while the sampler thread
    // is still running, with no explicit stop-then-reset ordering. Under TSan this would flag a
    // use-after-free with the old raw-pointer storage; with weak_ptr it's simply skipped.
    samplable.reset();
    std::this_thread::sleep_for(50ms); // Give the sampler thread several iterations to observe the expired weak_ptr

    sampler.stop();

    // If we get here without crashing (or a TSan report), the test passes.
    SUCCEED();
}

// =============================================================================
// Thread name (#843 measurement kit)
// =============================================================================

#if !defined(_WIN32)
namespace
{
class ThreadNameRecordingSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        std::array<char, 16> buffer{};
        const bool ok = pthread_getname_np(pthread_self(), buffer.data(), buffer.size()) == 0;
        const std::scoped_lock lock(m_Mutex);
        m_Name = ok ? std::string(buffer.data()) : std::string("<error>");
        m_Sampled = true;
        m_Cv.notify_all();
    }

    [[nodiscard]] std::string waitForName()
    {
        std::unique_lock lock(m_Mutex);
        if (!m_Cv.wait_for(lock, 2000ms, [this] { return m_Sampled; }))
        {
            ADD_FAILURE() << "Timeout waiting for a sample";
        }
        return m_Name;
    }

  private:
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::string m_Name;
    bool m_Sampled = false;
};
} // namespace

TEST(BackgroundSamplerTest, SamplerThreadCarriesConfiguredName)
{
    Domain::SamplerConfig config;
    config.interval = 100ms;
    config.threadName = "ts-test-sampler";
    Domain::BackgroundSampler sampler(config);
    const auto samplable = std::make_shared<ThreadNameRecordingSamplable>();
    sampler.addSamplable(samplable);
    sampler.start();
    EXPECT_EQ(samplable->waitForName(), "ts-test-sampler");
    sampler.stop();
}

TEST(BackgroundSamplerTest, SamplerThreadDefaultName)
{
    Domain::SamplerConfig config;
    config.interval = 100ms;
    Domain::BackgroundSampler sampler(config);
    const auto samplable = std::make_shared<ThreadNameRecordingSamplable>();
    sampler.addSamplable(samplable);
    sampler.start();
    EXPECT_EQ(samplable->waitForName(), Platform::SAMPLER_THREAD_NAME);
    sampler.stop();
}
#endif

// =============================================================================
// Scheduling, Overrun Backoff and Metrics (#1416)
// =============================================================================

namespace
{

using SteadyClock = std::chrono::steady_clock;
const SteadyClock::time_point PASS_START = SteadyClock::time_point{} + std::chrono::hours(1);

/// Sleeps for a fixed time in every sample() and records when each one started.
class SlowSamplable : public Domain::ISamplable
{
  public:
    explicit SlowSamplable(std::chrono::milliseconds duration) : m_Duration(duration)
    {}

    void sample() override
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_Starts.push_back(SteadyClock::now());
        }
        m_Cv.notify_all();
        std::this_thread::sleep_for(m_Duration);
    }

    [[nodiscard]] std::vector<SteadyClock::time_point> waitForStarts(std::size_t count)
    {
        std::unique_lock lock(m_Mutex);
        if (!m_Cv.wait_for(lock, 5s, [&] { return m_Starts.size() >= count; }))
        {
            ADD_FAILURE() << "Timeout waiting for " << count << " samples. Actual: " << m_Starts.size();
        }
        return m_Starts;
    }

  private:
    std::chrono::milliseconds m_Duration;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::vector<SteadyClock::time_point> m_Starts;
};

} // namespace

TEST(BackgroundSamplerScheduleTest, APassThatFitsKeepsTheFixedCadence)
{
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 30ms, 100ms), PASS_START + 100ms);
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 0ms, 1000ms), PASS_START + 1000ms);
}

TEST(BackgroundSamplerScheduleTest, APassTakingExactlyTheIntervalIsNotAnOverrun)
{
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 100ms, 100ms), PASS_START + 100ms);
}

TEST(BackgroundSamplerScheduleTest, AnOverrunRestsAsLongAsThePassTookBeforeTheNext)
{
    // Not straight away (the old start + interval was already past): after the pass ends, wait
    // as long as it took, so sampling takes at most about half a core.
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 300ms, 100ms), PASS_START + 300ms + 300ms);
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 1500ms, 1000ms), PASS_START + 1500ms + 1500ms);
}

TEST(BackgroundSamplerScheduleTest, TheBackoffIsCappedAtTheSlowestSupportedInterval)
{
    const auto maxGap = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS);
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, 20s, 1000ms), PASS_START + 20s + maxGap);
    EXPECT_EQ(Domain::nextSampleTime(PASS_START, maxGap + 1ms, 100ms), PASS_START + maxGap + 1ms + maxGap);
}

TEST(BackgroundSamplerScheduleTest, TheCadenceResumesWithThePassAfterAnOverrun)
{
    // The decision is per pass: no state carries over from an overrun.
    const auto afterOverrun = Domain::nextSampleTime(PASS_START, 400ms, 100ms);
    EXPECT_EQ(Domain::nextSampleTime(afterOverrun, 20ms, 100ms), afterOverrun + 100ms);
}

TEST(BackgroundSamplerScheduleTest, TheNextPassNeverStartsBeforeThisOneEnds)
{
    for (const auto duration : {0ms, 99ms, 100ms, 101ms, 250ms, 4999ms, 5000ms, 12000ms})
    {
        EXPECT_GE(Domain::nextSampleTime(PASS_START, duration, 100ms), PASS_START + duration) << duration.count() << " ms";
    }
}

TEST(BackgroundSamplerTest, AnOverrunningSamplerBacksOffInsteadOfSamplingBackToBack)
{
    // A 150 ms sample at the 100 ms interval: before #1416 the next pass started the moment one
    // finished (starts 150 ms apart, a core kept busy). Now each start waits for the previous pass
    // to end plus as long again: at least 300 ms apart.
    constexpr auto SAMPLE_TIME = 150ms;
    Domain::SamplerConfig config;
    config.interval = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    Domain::BackgroundSampler sampler(config);
    const auto samplable = std::make_shared<SlowSamplable>(SAMPLE_TIME);
    sampler.addSamplable(samplable, "slow");
    sampler.start();
    const auto starts = samplable->waitForStarts(3);
    sampler.stop();

    ASSERT_GE(starts.size(), 3U);
    for (std::size_t i = 1; i < starts.size(); ++i)
    {
        EXPECT_GE(starts[i] - starts[i - 1], 2 * SAMPLE_TIME - 5ms) << "between samples " << (i - 1) << " and " << i;
    }

    const auto metrics = sampler.metrics();
    EXPECT_GE(metrics.backoffs, 2U);
    EXPECT_GE(metrics.pass.overruns, 2U);
    ASSERT_EQ(metrics.samplables.size(), 1U);
    EXPECT_EQ(metrics.samplables[0].name, "slow");
    EXPECT_GE(metrics.samplables[0].overruns, 2U);
    EXPECT_GE(metrics.samplables[0].lastDuration, SAMPLE_TIME);
}

TEST(BackgroundSamplerTest, MetricsRecordEachSamplablesDurationWithoutOverruns)
{
    constexpr auto SAMPLE_TIME = 20ms;
    Domain::SamplerConfig config;
    config.interval = 200ms;
    Domain::BackgroundSampler sampler(config);
    const auto slow = std::make_shared<SlowSamplable>(SAMPLE_TIME);
    const auto fast = std::make_shared<MockSamplable>();
    sampler.addSamplable(slow, "storage");
    sampler.addSamplable(fast); // unnamed: labelled by position
    sampler.start();
    static_cast<void>(slow->waitForStarts(2));
    fast->waitForSamples(2);
    ASSERT_TRUE(waitFor([&] { return sampler.metrics().pass.samples >= 2; }));
    sampler.stop();

    const auto metrics = sampler.metrics();
    ASSERT_EQ(metrics.samplables.size(), 2U);
    EXPECT_EQ(metrics.samplables[0].name, "storage");
    EXPECT_EQ(metrics.samplables[1].name, "samplable 1");
    EXPECT_GE(metrics.samplables[0].samples, 2U);
    EXPECT_GE(metrics.samplables[0].lastDuration, SAMPLE_TIME);
    EXPECT_GE(metrics.samplables[0].maxDuration, metrics.samplables[0].lastDuration);
    EXPECT_GE(metrics.pass.maxDuration, metrics.samplables[0].maxDuration);
    EXPECT_EQ(metrics.samplables[0].overruns, 0U);
    EXPECT_EQ(metrics.pass.overruns, 0U);
    EXPECT_EQ(metrics.backoffs, 0U);
}

TEST(BackgroundSamplerTest, MetricsStartOverOnRestart)
{
    Domain::SamplerConfig config;
    config.interval = 100ms;
    Domain::BackgroundSampler sampler(config);
    const auto samplable = std::make_shared<MockSamplable>();
    sampler.addSamplable(samplable);
    sampler.start();
    samplable->waitForSamples(1);
    ASSERT_TRUE(waitFor([&] { return sampler.metrics().pass.samples >= 1; }));
    sampler.stop();

    sampler.start();
    EXPECT_LE(sampler.metrics().pass.samples, 1U); // reset at start(); the first new pass may already have run
    sampler.stop();
}
