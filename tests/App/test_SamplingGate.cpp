/// @file test_SamplingGate.cpp
/// @brief App::SamplingGate (#800): the Services panel's sampler reads nothing while the tab is
/// hidden, and hiding it never has to stop (join) the sampler thread.

#include "App/Panels/SamplingGate.h"
#include "Domain/BackgroundSampler.h"
#include "Domain/ISamplable.h"
#include "Domain/PublicationWake.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace App
{
namespace
{

using namespace std::chrono_literals;

class CountingSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        ++count;
    }

    int count = 0;
};

/// A target that is sampled but has nothing new to show.
class IdleSamplable final : public CountingSamplable
{
  public:
    [[nodiscard]] bool lastSampleMayHavePublished() const noexcept override
    {
        return false;
    }
};

TEST(SamplingGateTest, ForwardsOnlyWhileOpen)
{
    const auto target = std::make_shared<CountingSamplable>();
    SamplingGate gate(target);

    EXPECT_FALSE(gate.isOpen()); // closed until the tab first shows
    gate.sample();
    EXPECT_EQ(target->count, 0);

    gate.setOpen(true);
    gate.sample();
    gate.sample();
    EXPECT_EQ(target->count, 2);

    gate.setOpen(false);
    gate.sample();
    EXPECT_EQ(target->count, 2);
}

TEST(SamplingGateTest, SaysWhetherItsLastSampleReachedTheTarget)
{
    const auto target = std::make_shared<CountingSamplable>();
    SamplingGate gate(target);

    gate.sample(); // closed: read nothing, so its sampler's pass doesn't wake the frame loop (#3285)
    EXPECT_FALSE(gate.lastSampleMayHavePublished());

    gate.setOpen(true);
    gate.sample();
    EXPECT_TRUE(gate.lastSampleMayHavePublished());

    gate.setOpen(false);
    gate.sample();
    EXPECT_FALSE(gate.lastSampleMayHavePublished());
}

TEST(SamplingGateTest, ATargetThatPublishedNothingIsReportedThrough)
{
    const auto target = std::make_shared<IdleSamplable>();
    SamplingGate gate(target);
    gate.setOpen(true);
    gate.sample();
    EXPECT_EQ(target->count, 1);
    EXPECT_FALSE(gate.lastSampleMayHavePublished());
}

TEST(SamplingGateTest, AClosedGateDoesNotWakeItsSamplersFrameLoop)
{
    static std::atomic<int> wakes{0};
    wakes = 0;
    const auto previous = Domain::PublicationWake::setHandler([] noexcept { wakes.fetch_add(1); });
    const auto target = std::make_shared<CountingSamplable>();
    const auto gate = std::make_shared<SamplingGate>(target);
    Domain::SamplerConfig config;
    config.interval = 50ms;
    Domain::BackgroundSampler sampler(config);
    sampler.addSamplable(gate);
    sampler.start();
    std::this_thread::sleep_for(300ms); // several passes, every one through the closed gate
    sampler.stop();
    EXPECT_EQ(wakes.load(), 0);
    EXPECT_EQ(target->count, 0);
    static_cast<void>(Domain::PublicationWake::setHandler(previous));
}

TEST(SamplingGateTest, ATargetReleasedByItsOwnerIsSkipped)
{
    auto target = std::make_shared<CountingSamplable>();
    SamplingGate gate(target);
    gate.setOpen(true);
    target.reset();
    gate.sample(); // no target left: nothing to do, and no crash
    EXPECT_TRUE(gate.isOpen());
}

} // namespace
} // namespace App
