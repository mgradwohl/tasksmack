/// @file test_PowerProbeContract.cpp
/// @brief Cross-platform contract tests for Platform::IPowerProbe via Platform::makePowerProbe()

#include "Platform/Factory.h"
#include "Platform/IPowerProbe.h"
#include "Platform/PowerTypes.h"

#include <gtest/gtest.h>

namespace Platform
{

TEST(PowerProbeContractTest, FactoryConstructs)
{
    auto probe = makePowerProbe();
    ASSERT_NE(probe, nullptr);
}

TEST(PowerProbeContractTest, CapabilitiesAreValid)
{
    auto probe = makePowerProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();

    // Capabilities are hardware-dependent, but structure should be valid
    // If no battery, most capabilities should be false
    if (!caps.hasBattery)
    {
        EXPECT_FALSE(caps.hasChargePercent);
        EXPECT_FALSE(caps.hasChargeCapacity);
        EXPECT_FALSE(caps.hasPowerRate);
        EXPECT_FALSE(caps.hasTimeEstimates);
    }
}

TEST(PowerProbeContractTest, ReadReturnsSaneCounters)
{
    auto probe = makePowerProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    const PowerCounters counters = probe->read();

    // Basic state validation
    if (!caps.hasBattery)
    {
        // No battery: should report NotPresent and be on AC
        EXPECT_EQ(counters.state, BatteryState::NotPresent);
        EXPECT_TRUE(counters.isOnAc);
    }
    else
    {
        // Battery present: state should be valid
        EXPECT_TRUE(counters.state == BatteryState::Unknown || counters.state == BatteryState::Charging ||
                    counters.state == BatteryState::Discharging || counters.state == BatteryState::Full);

        // Charge percent validation if supported
        if (caps.hasChargePercent)
        {
            EXPECT_TRUE(counters.chargePercent == -1 || (counters.chargePercent >= 0 && counters.chargePercent <= 100))
                << "Charge percent: " << counters.chargePercent;
        }

        // Health percent validation if supported
        if (caps.hasHealthPercent)
        {
            EXPECT_TRUE(counters.healthPercent == -1 || (counters.healthPercent >= 0 && counters.healthPercent <= 100))
                << "Health percent: " << counters.healthPercent;
        }

        // Capacity validation if supported
        if (caps.hasChargeCapacity)
        {
            EXPECT_LE(counters.chargeNowWh, counters.chargeFullWh);
        }
    }
}

TEST(PowerProbeContractTest, MultipleReadsSucceed)
{
    auto probe = makePowerProbe();
    ASSERT_NE(probe, nullptr);

    // Should be able to read multiple times without errors
    PowerCounters counters1;
    PowerCounters counters2;

    EXPECT_NO_THROW({ counters1 = probe->read(); });
    EXPECT_NO_THROW({ counters2 = probe->read(); });

    // State should be consistent (or at least valid) across reads
    auto caps = probe->capabilities();
    if (!caps.hasBattery)
    {
        EXPECT_EQ(counters1.state, BatteryState::NotPresent);
        EXPECT_EQ(counters2.state, BatteryState::NotPresent);
    }
}

TEST(PowerProbeContractTest, StateIsConsistentWithAcStatus)
{
    auto probe = makePowerProbe();
    ASSERT_NE(probe, nullptr);

    const auto caps = probe->capabilities();
    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected";
    }

    const PowerCounters counters = probe->read();

    // If on AC and fully charged, should be Charging or Full
    // If not on AC, should be Discharging
    if (!counters.isOnAc)
    {
        EXPECT_TRUE(counters.state == BatteryState::Discharging || counters.state == BatteryState::Unknown);
    }
}

} // namespace Platform
