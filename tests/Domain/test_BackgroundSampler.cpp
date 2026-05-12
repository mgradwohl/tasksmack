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
#include "Mocks/MockProbes.h"
#include "Platform/ProcessTypes.h"

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

// Use shared mock from TestMocks namespace
using TestMocks::MockProcessProbe;

// =============================================================================
// Construction Tests
// =============================================================================

TEST(BackgroundSamplerTest, ConstructWithValidProbe)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, ConstructWithCustomInterval)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::SamplerConfig config;
    config.interval = 500ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    EXPECT_EQ(sampler.interval(), 500ms);
}

TEST(BackgroundSamplerTest, DefaultIntervalIsOneSecond)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    EXPECT_EQ(sampler.interval(), 1000ms);
}

// =============================================================================
// Start/Stop Lifecycle Tests
// =============================================================================

TEST(BackgroundSamplerTest, StartSetsRunningTrue)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    sampler.start();
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, StopWhenNotRunningIsNoOp)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    // Should not crash
    sampler.stop();
    EXPECT_FALSE(sampler.isRunning());
}

TEST(BackgroundSamplerTest, DoubleStartIsIgnored)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Domain::BackgroundSampler sampler(std::move(probe));

    sampler.start();
    sampler.start(); // Should be ignored
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
}

TEST(BackgroundSamplerTest, DestructorStopsSampler)
{
    auto probe = std::make_unique<MockProcessProbe>();
    {
        Domain::BackgroundSampler sampler(std::move(probe));
        sampler.start();
        EXPECT_TRUE(sampler.isRunning());
        // Destructor should stop the sampler
    }
    // If we get here without hanging, the test passes
    SUCCEED();
}

// =============================================================================
// Callback Tests
// =============================================================================

TEST(BackgroundSamplerTest, CallbackInvokedOnSample)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    Platform::ProcessCounters pc;
    pc.pid = 123;
    pc.name = "test_process";
    rawProbe->setCounters({pc});
    rawProbe->setTotalCpuTime(10000);

    Domain::SamplerConfig config;
    config.interval = 50ms; // Fast sampling for test

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::mutex mtx;
    std::condition_variable cv;
    bool callbackCalled = false;
    std::vector<Platform::ProcessCounters> receivedCounters;
    uint64_t receivedTotalCpu = 0;

    sampler.setCallback(
        [&](const std::vector<Platform::ProcessCounters>& counters, uint64_t totalCpu)
        {
            std::lock_guard lock(mtx);
            receivedCounters = counters;
            receivedTotalCpu = totalCpu;
            callbackCalled = true;
            cv.notify_one();
        });

    sampler.start();

    // Wait for callback with timeout
    {
        std::unique_lock lock(mtx);
        EXPECT_TRUE(cv.wait_for(lock, 500ms, [&] { return callbackCalled; }));
    }

    sampler.stop();

    EXPECT_TRUE(callbackCalled);
    ASSERT_EQ(receivedCounters.size(), 1);
    EXPECT_EQ(receivedCounters[0].pid, 123);
    EXPECT_EQ(receivedCounters[0].name, "test_process");
    EXPECT_EQ(receivedTotalCpu, 10000);
}

TEST(BackgroundSamplerTest, CallbackInvokedMultipleTimes)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 30ms; // Fast sampling for test

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};
    sampler.setCallback([&](const auto&, uint64_t) { callbackCount.fetch_add(1); });

    sampler.start();

    // Wait for multiple callbacks
    std::this_thread::sleep_for(200ms);

    sampler.stop();

    // Should have been called multiple times (at least 3-4 times in 200ms with 30ms interval)
    EXPECT_GE(callbackCount.load(), 3);
}

TEST(BackgroundSamplerTest, NoCallbackSetDoesNotCrash)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    // Don't set a callback
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
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 500ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    sampler.start();
    EXPECT_EQ(sampler.interval(), 500ms);

    sampler.setInterval(100ms);
    EXPECT_EQ(sampler.interval(), 100ms);

    sampler.stop();
}

TEST(BackgroundSamplerTest, SetIntervalWhileStopped)
{
    auto probe = std::make_unique<MockProcessProbe>();

    Domain::BackgroundSampler sampler(std::move(probe));

    EXPECT_EQ(sampler.interval(), 1000ms);
    sampler.setInterval(250ms);
    EXPECT_EQ(sampler.interval(), 250ms);
}

// =============================================================================
// Refresh Request Tests
// =============================================================================

TEST(BackgroundSamplerTest, RequestRefreshTriggersEarlySample)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();

    rawProbe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 10000ms; // Long interval

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};
    sampler.setCallback([&](const auto&, uint64_t) { callbackCount.fetch_add(1); });

    sampler.start();

    // Wait for first sample
    std::this_thread::sleep_for(100ms);
    int countAfterFirst = callbackCount.load();
    EXPECT_GE(countAfterFirst, 1);

    // Request refresh - should trigger another sample quickly
    sampler.requestRefresh();
    std::this_thread::sleep_for(100ms);

    sampler.stop();

    // Should have gotten at least one more sample after refresh request
    EXPECT_GT(callbackCount.load(), countAfterFirst);
}

// =============================================================================
// Capabilities Passthrough Tests
// =============================================================================

TEST(BackgroundSamplerTest, CapabilitiesPassedFromProbe)
{
    auto probe = std::make_unique<MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasIoCounters = true;
    caps.hasThreadCount = true;
    caps.hasUserSystemTime = true;
    caps.hasStartTime = true;
    probe->setCapabilities(caps);

    Domain::BackgroundSampler sampler(std::move(probe));

    const auto& samplerCaps = sampler.capabilities();
    EXPECT_TRUE(samplerCaps.hasIoCounters);
    EXPECT_TRUE(samplerCaps.hasThreadCount);
    EXPECT_TRUE(samplerCaps.hasUserSystemTime);
    EXPECT_TRUE(samplerCaps.hasStartTime);
}

TEST(BackgroundSamplerTest, TicksPerSecondPassedFromProbe)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setTicksPerSecond(250);

    Domain::BackgroundSampler sampler(std::move(probe));

    EXPECT_EQ(sampler.ticksPerSecond(), 250);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(BackgroundSamplerTest, ConcurrentIntervalChanges)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);
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

TEST(BackgroundSamplerTest, ConcurrentCallbackChange)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 30ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};

    sampler.start();

    // Change callback while sampler is running
    for (int i = 0; i < 10; ++i)
    {
        sampler.setCallback([&](const auto&, uint64_t) { callbackCount.fetch_add(1); });
        std::this_thread::sleep_for(20ms);
    }

    sampler.stop();

    // Should have invoked callbacks without crashing
    EXPECT_GT(callbackCount.load(), 0);
}

TEST(BackgroundSamplerTest, ConcurrentRefreshRequests)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 200ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);
    sampler.setCallback([](const auto&, uint64_t) {});

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

    // Should have enumerated at least a few times (timing-dependent, so relaxed check)
    EXPECT_GT(rawProbe->enumerateCount(), 1);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(BackgroundSamplerTest, VeryShortInterval)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 1ms; // Very short

    Domain::BackgroundSampler sampler(std::move(probe), config);
    sampler.setCallback([](const auto&, uint64_t) {});

    sampler.start();
    std::this_thread::sleep_for(100ms);
    sampler.stop();

    // Should have enumerated many times (relaxed to avoid timing flakiness)
    EXPECT_GE(rawProbe->enumerateCount(), 5);
}

TEST(BackgroundSamplerTest, StartStopStartCycle)
{
    auto probe = std::make_unique<MockProcessProbe>();
    auto* rawProbe = probe.get();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);
    sampler.setCallback([](const auto&, uint64_t) {});

    // Start/stop cycle multiple times
    for (int i = 0; i < 3; ++i)
    {
        sampler.start();
        EXPECT_TRUE(sampler.isRunning());
        std::this_thread::sleep_for(100ms);
        sampler.stop();
        EXPECT_FALSE(sampler.isRunning());
    }

    // Should have enumerated multiple times across all cycles
    EXPECT_GT(rawProbe->enumerateCount(), 3);
}

TEST(BackgroundSamplerTest, EmptyProcessList)
{
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({}); // Empty list

    Domain::SamplerConfig config;
    config.interval = 50ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};
    sampler.setCallback(
        [&](const std::vector<Platform::ProcessCounters>& counters, uint64_t)
        {
            EXPECT_TRUE(counters.empty());
            callbackCount.fetch_add(1);
        });

    sampler.start();
    std::this_thread::sleep_for(150ms);
    sampler.stop();

    EXPECT_GT(callbackCount.load(), 0);
}

TEST(BackgroundSamplerTest, VeryShortIntervalStillWorks)
{
    // Test the branch where sleepTime <= 0 (interval shorter than enumerate time)
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 1ms; // Extremely short interval

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};
    sampler.setCallback([&](const auto&, uint64_t) { callbackCount.fetch_add(1); });

    sampler.start();
    std::this_thread::sleep_for(100ms);
    sampler.stop();

    // Should have been called multiple times even with very short interval (relaxed to avoid timing flakiness)
    EXPECT_GE(callbackCount.load(), 4);
}

TEST(BackgroundSamplerTest, ZeroIntervalHandledGracefully)
{
    // Edge case: zero interval
    auto probe = std::make_unique<MockProcessProbe>();
    probe->setCounters({});

    Domain::SamplerConfig config;
    config.interval = 0ms; // Zero interval - sleepTime will always be <= 0

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callbackCount{0};
    sampler.setCallback([&](const auto&, uint64_t) { callbackCount.fetch_add(1); });

    sampler.start();
    std::this_thread::sleep_for(50ms);
    sampler.stop();

    // Should have been called many times with zero interval
    EXPECT_GT(callbackCount.load(), 5);
}

// =============================================================================
// Exception Safety Tests
// =============================================================================

/// A process probe that throws std::runtime_error from enumerate() on every call.
/// Used to verify the sampler loop handles probe exceptions gracefully.
class ThrowingProcessProbe : public Platform::IProcessProbe
{
  public:
    [[nodiscard]] std::vector<Platform::ProcessCounters> enumerate() override
    {
        m_EnumerateCount.fetch_add(1);
        throw std::runtime_error("simulated probe enumerate failure");
    }

    [[nodiscard]] uint64_t totalCpuTime() const override
    {
        return 0;
    }

    [[nodiscard]] uint64_t systemTotalMemory() const override
    {
        return 0;
    }

    [[nodiscard]] Platform::ProcessCapabilities capabilities() const override
    {
        return {};
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return 100;
    }

    [[nodiscard]] int enumerateCount() const
    {
        return m_EnumerateCount.load();
    }

  private:
    std::atomic<int> m_EnumerateCount{0};
};

TEST(BackgroundSamplerTest, ProbeEnumerateThrowingSamplerContinues)
{
    // Verify that if the probe's enumerate() throws, the sampler thread
    // catches the exception and continues running rather than terminating.
    auto probe = std::make_unique<ThrowingProcessProbe>();
    ThrowingProcessProbe* rawProbe = probe.get(); // borrow raw pointer before move

    Domain::SamplerConfig config;
    config.interval = 10ms; // Fast interval to accumulate multiple exceptions quickly

    Domain::BackgroundSampler sampler(std::move(probe), config);
    sampler.start();

    std::this_thread::sleep_for(100ms);

    // Sampler should still be running despite repeated exceptions from the probe
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();

    // Probe's enumerate should have been called multiple times (sampler kept looping)
    EXPECT_GT(rawProbe->enumerateCount(), 1);
}

TEST(BackgroundSamplerTest, CallbackThrowingExceptionSamplerContinues)
{
    // Verify that if the registered callback throws, the sampler thread
    // catches the exception and continues running.
    auto probe = std::make_unique<MockProcessProbe>();
    probe->withProcess(1, "test");

    Domain::SamplerConfig config;
    config.interval = 10ms;

    Domain::BackgroundSampler sampler(std::move(probe), config);

    std::atomic<int> callCount{0};
    sampler.setCallback(
        [&](const auto&, uint64_t)
        {
            callCount.fetch_add(1);
            throw std::runtime_error("simulated callback failure");
        });

    sampler.start();
    std::this_thread::sleep_for(100ms);

    // Sampler should still be running despite repeated callback exceptions
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();

    // Callback should have been called multiple times (sampler kept looping after each throw)
    EXPECT_GT(callCount.load(), 1);
}

// ========== Unknown exception handling (catch(...) branch L172) ==========

/// A probe that throws a non-std::exception (integer literal) to exercise the
/// catch(...) branch in the sampler loop.
class ThrowingNonStdProbe : public Platform::IProcessProbe
{
  public:
    explicit ThrowingNonStdProbe(std::shared_ptr<std::atomic<int>> counter) : m_CallCount(std::move(counter))
    {}

    [[nodiscard]] std::vector<Platform::ProcessCounters> enumerate() override
    {
        ++(*m_CallCount);
        // NOLINTNEXTLINE(hicpp-exception-baseclass) — intentionally non-std for coverage
        throw 42; // non-std::exception to hit catch(...) branch
    }

    [[nodiscard]] uint64_t totalCpuTime() const override
    {
        return 0;
    }

    [[nodiscard]] Platform::ProcessCapabilities capabilities() const override
    {
        return {};
    }

    [[nodiscard]] long ticksPerSecond() const override
    {
        return 100;
    }

    [[nodiscard]] uint64_t systemTotalMemory() const override
    {
        return 0;
    }

  private:
    std::shared_ptr<std::atomic<int>> m_CallCount;
};

TEST(BackgroundSamplerTest, ProbeThrowingNonStdExceptionSamplerContinues)
{
    // Verify the catch(...) branch in the sampler loop keeps the thread alive.
    // A shared atomic counter lets us confirm at least one enumerate() call happened
    // before asserting, avoiding the flakiness of a single fixed-duration sleep.
    auto callCount = std::make_shared<std::atomic<int>>(0);

    Domain::SamplerConfig config;
    config.interval = 10ms;

    Domain::BackgroundSampler sampler(std::make_unique<ThrowingNonStdProbe>(callCount), config);

    sampler.start();

    // Wait until at least one enumerate() call has occurred, confirming the loop ran
    constexpr auto kMaxWait = 2000ms;
    constexpr auto kPollInterval = 5ms;
    auto waited = 0ms;
    while (callCount->load() == 0 && waited < kMaxWait)
    {
        std::this_thread::sleep_for(kPollInterval);
        waited += kPollInterval;
    }

    EXPECT_GT(callCount->load(), 0);
    EXPECT_TRUE(sampler.isRunning());

    sampler.stop();
}
