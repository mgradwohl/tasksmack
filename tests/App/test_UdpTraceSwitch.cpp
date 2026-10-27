/// @file test_UdpTraceSwitch.cpp
/// @brief App::UdpTraceSwitch (#3155): the Settings toggle's start and stop of the UDP event trace run
/// on a worker, never on the UI thread. A request returns while the switch still runs (pending), its
/// outcome is taken in once by update() (completion), a start the OS refuses or a collector that throws
/// is an outcome that says so (error), and a request while one runs waits for it -- the last wish wins,
/// and one that is already the collector's state starts nothing. A fake collector stands in for ETW: no
/// real trace session is ever started.

#include "App/Panels/UdpTraceSwitch.h"
#include "Platform/IUdpTrafficCollector.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace App
{
namespace
{

constexpr std::string_view THREAD_NAME = "ts-test-udp";

/// What the switch called, in order: "start" or "stop".
using Calls = std::vector<std::string>;

/// An IUdpTrafficCollector whose start() and stop() can be held, as ETW's sweep and the consumer's join
/// can take long, and can be made to refuse or throw. Records each call and the thread it ran on.
class FakeCollector final : public Platform::IUdpTrafficCollector
{
  public:
    FakeCollector() : m_Released(m_Release.get_future().share())
    {}

    ~FakeCollector() override
    {
        release();
    }

    FakeCollector(const FakeCollector&) = delete;
    FakeCollector& operator=(const FakeCollector&) = delete;
    FakeCollector(FakeCollector&&) = delete;
    FakeCollector& operator=(FakeCollector&&) = delete;

    [[nodiscard]] Platform::UdpCollectorStartResult start() override
    {
        record("start");
        if (m_Throw)
        {
            throw std::runtime_error("StartTraceW broke");
        }
        return m_StartResult;
    }

    void stop() override
    {
        record("stop");
    }

    [[nodiscard]] bool isDelivering() const override
    {
        return false;
    }

    [[nodiscard]] std::vector<Platform::UdpProcessTraffic> sample() override
    {
        return {};
    }

    /// From now on start() and stop() wait until release() (bounded, so a failed test tears down).
    void hold()
    {
        m_Hold = true;
    }
    void release()
    {
        if (!m_IsReleased)
        {
            m_IsReleased = true;
            m_Release.set_value();
        }
    }
    void setStartResult(Platform::UdpCollectorStartResult result)
    {
        m_StartResult = result;
    }
    void throwOnStart()
    {
        m_Throw = true;
    }

    [[nodiscard]] Calls calls() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Calls;
    }
    [[nodiscard]] bool ranOnCallingThread() const
    {
        return m_OnCallingThread.load();
    }
    /// Waits (at most a few seconds) until the worker has made @p count calls: one held is under way.
    [[nodiscard]] bool waitForCalls(std::size_t count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (calls().size() < count)
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

  private:
    void record(const char* call)
    {
        {
            const std::scoped_lock lock(m_Mutex);
            m_Calls.emplace_back(call);
        }
        if (std::this_thread::get_id() == m_CallingThread)
        {
            m_OnCallingThread.store(true);
        }
        if (m_Hold)
        {
            static_cast<void>(m_Released.wait_for(std::chrono::seconds(30)));
        }
    }

    // Set by the test thread before a switch starts, read by the worker after (std::async orders them).
    bool m_Hold = false;
    bool m_Throw = false;
    Platform::UdpCollectorStartResult m_StartResult = Platform::UdpCollectorStartResult::Started;
    std::promise<void> m_Release;
    std::shared_future<void> m_Released;
    bool m_IsReleased = false; // the test thread's
    const std::thread::id m_CallingThread = std::this_thread::get_id();
    std::atomic<bool> m_OnCallingThread{false};
    mutable std::mutex m_Mutex;
    Calls m_Calls; // Guarded by m_Mutex
};

TEST(UdpTraceSwitchTest, AStartRunsOnTheWorkerAndItsOutcomeIsTakenInOnce)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->hold();
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.pending());

    // Pending: the request returns while the start still runs; nothing to take in yet.
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());
    EXPECT_TRUE(traceSwitch.pending());
    EXPECT_FALSE(traceSwitch.update().has_value());
    EXPECT_TRUE(traceSwitch.pending());

    // Completion.
    collector->release();
    const std::optional<UdpTraceSwitch::Outcome> outcome = traceSwitch.update(/*wait=*/true);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->enabled);
    EXPECT_EQ(outcome->started, Platform::UdpCollectorStartResult::Started);
    EXPECT_TRUE(outcome->error.empty());
    EXPECT_FALSE(traceSwitch.pending());
    EXPECT_FALSE(traceSwitch.update(true).has_value()) << "taken in once";
    EXPECT_EQ(collector->calls(), Calls{"start"});
    EXPECT_FALSE(collector->ranOnCallingThread()) << "never on the UI thread";
}

TEST(UdpTraceSwitchTest, AStopRunsOnTheWorkerToo)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->hold(); // a stop's join of a consumer that won't end
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, false).has_value());
    EXPECT_TRUE(traceSwitch.pending()) << "the request returned while the stop still runs";
    collector->release();
    const std::optional<UdpTraceSwitch::Outcome> outcome = traceSwitch.update(true);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_FALSE(outcome->enabled);
    EXPECT_FALSE(outcome->started.has_value());
    EXPECT_TRUE(outcome->error.empty());
    EXPECT_EQ(collector->calls(), Calls{"stop"});
    EXPECT_FALSE(collector->ranOnCallingThread());
}

TEST(UdpTraceSwitchTest, ARefusedStartSaysWhatTheCollectorSaid)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->setStartResult(Platform::UdpCollectorStartResult::NotElevated);
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());
    const std::optional<UdpTraceSwitch::Outcome> outcome = traceSwitch.update(true);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->started, Platform::UdpCollectorStartResult::NotElevated);
    EXPECT_TRUE(outcome->error.empty()) << "the collector logs why; the switch itself ran";
}

TEST(UdpTraceSwitchTest, ACollectorThatThrowsIsAnOutcomeThatSaysWhy)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->throwOnStart();
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());
    std::optional<UdpTraceSwitch::Outcome> outcome;
    EXPECT_NO_THROW(outcome = traceSwitch.update(true));
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->error, "StartTraceW broke");
    EXPECT_FALSE(outcome->started.has_value());
    EXPECT_FALSE(traceSwitch.pending());
}

TEST(UdpTraceSwitchTest, ARequestWhileOneRunsWaitsForItAndTheLastWishWins)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->hold();
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());  // Save with the toggle on...
    EXPECT_FALSE(traceSwitch.request(collector, false).has_value()); // ...off again while it starts...
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());  // ...and on: nothing more to do
    EXPECT_FALSE(traceSwitch.request(collector, false).has_value()); // ...and off: the stop waits its turn
    ASSERT_TRUE(collector->waitForCalls(1));                         // the start is under way on the worker, held
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(collector->calls(), Calls{"start"}) << "never two at once";
    EXPECT_TRUE(traceSwitch.pending());

    collector->release();
    // The start is taken in, and the stop asked for meanwhile started in the same update().
    const std::optional<UdpTraceSwitch::Outcome> first = traceSwitch.update(true);
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->enabled);
    EXPECT_TRUE(traceSwitch.pending());
    const std::optional<UdpTraceSwitch::Outcome> second = traceSwitch.update(true);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->enabled);
    EXPECT_FALSE(traceSwitch.pending());
    EXPECT_EQ(collector->calls(), (Calls{"start", "stop"}));
}

TEST(UdpTraceSwitchTest, AWishThatIsAlreadyTheCollectorsStateStartsNothing)
{
    auto collector = std::make_shared<FakeCollector>();
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());
    ASSERT_TRUE(traceSwitch.update(true).has_value());
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value()); // the same again
    EXPECT_FALSE(traceSwitch.pending());
    EXPECT_FALSE(traceSwitch.update(true).has_value());
    EXPECT_EQ(collector->calls(), Calls{"start"});
}

TEST(UdpTraceSwitchTest, NoCollectorNothingToSwitch)
{
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(nullptr, true).has_value());
    EXPECT_FALSE(traceSwitch.pending());
    EXPECT_FALSE(traceSwitch.update(true).has_value());
}

TEST(UdpTraceSwitchTest, ShutdownWaitsForTheSwitchRunningAndDropsTheRest)
{
    auto collector = std::make_shared<FakeCollector>();
    collector->hold();
    UdpTraceSwitch traceSwitch(THREAD_NAME);
    EXPECT_FALSE(traceSwitch.request(collector, true).has_value());
    EXPECT_FALSE(traceSwitch.request(collector, false).has_value()); // waiting its turn
    std::thread releaser(
        [collector]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            collector->release();
        });
    const std::optional<UdpTraceSwitch::Outcome> outcome = traceSwitch.shutdown(); // the panel's detach
    releaser.join();
    ASSERT_TRUE(outcome.has_value());
    EXPECT_TRUE(outcome->enabled);
    EXPECT_FALSE(traceSwitch.pending()) << "the stop asked for after it is dropped: the panel stops it itself";
    EXPECT_FALSE(traceSwitch.update(true).has_value());
    EXPECT_EQ(collector->calls(), Calls{"start"});
}

TEST(UdpTraceSwitchTest, TheDestructorWaitsForTheSwitchRunning)
{
    auto collector = std::make_shared<FakeCollector>();
    {
        UdpTraceSwitch traceSwitch(THREAD_NAME);
        EXPECT_FALSE(traceSwitch.request(collector, false).has_value());
    }
    EXPECT_EQ(collector->calls(), Calls{"stop"}) << "done before the switch went";
}

TEST(UdpTraceSwitchTest, TheDefaultWorkerHasItsOwnShortName)
{
    EXPECT_EQ(Platform::UDP_TRACE_SWITCH_THREAD_NAME, "ts-udp-switch");
    EXPECT_LE(Platform::UDP_TRACE_SWITCH_THREAD_NAME.size(), Platform::MAX_LINUX_THREAD_NAME_LENGTH);
}

} // namespace
} // namespace App
