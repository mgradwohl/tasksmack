/// @file test_SamplingGate.cpp
/// @brief App::SamplingGate (#800): the Services panel's sampler reads nothing while the tab is
/// hidden, and hiding it never has to stop (join) the sampler thread.

#include "App/Panels/SamplingGate.h"
#include "Domain/ISamplable.h"

#include <gtest/gtest.h>

#include <memory>

namespace App
{
namespace
{

class CountingSamplable : public Domain::ISamplable
{
  public:
    void sample() override
    {
        ++count;
    }

    int count = 0;
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
