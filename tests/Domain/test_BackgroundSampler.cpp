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

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

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
    std::this_thread::sleep_for(100ms);
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
        sampler.start();
        EXPECT_TRUE(sampler.isRunning());
        std::this_thread::sleep_for(100ms);
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
