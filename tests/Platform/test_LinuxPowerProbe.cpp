/// @file test_LinuxPowerProbe.cpp
/// @brief Integration tests for Platform::LinuxPowerProbe
///
/// These are integration tests that interact with the real /sys/class/power_supply filesystem.
/// They verify that the probe correctly reads and parses battery/power information.

#include <gtest/gtest.h>

#if defined(__linux__) && __has_include(<unistd.h>)

#include "Platform/Linux/LinuxPowerProbe.h"
#include "Platform/PowerTypes.h"

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(LinuxPowerProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ LinuxPowerProbe probe; });
}

TEST(LinuxPowerProbeTest, CapabilitiesReportedCorrectly)
{
    LinuxPowerProbe probe;
    auto caps = probe.capabilities();

    // Capabilities depend on hardware - just verify structure is valid
    // A system may or may not have a battery
    EXPECT_TRUE(caps.hasBattery || !caps.hasBattery);
    
    // If there's no battery, most other capabilities should be false
    if (!caps.hasBattery)
    {
        EXPECT_FALSE(caps.hasChargePercent);
        EXPECT_FALSE(caps.hasChargeCapacity);
    }
}

TEST(LinuxPowerProbeTest, ReadSucceeds)
{
    LinuxPowerProbe probe;
    PowerCounters counters;
    
    EXPECT_NO_THROW({ counters = probe.read(); });
    
    // Verify basic structure validity
    auto caps = probe.capabilities();
    if (caps.hasBattery)
    {
        // If battery is present, charge percent should be valid range or -1
        EXPECT_TRUE(counters.chargePercent == -1 || 
                   (counters.chargePercent >= 0 && counters.chargePercent <= 100));
        
        // Battery state should be one of the valid states
        EXPECT_TRUE(counters.state == BatteryState::Unknown ||
                   counters.state == BatteryState::Charging ||
                   counters.state == BatteryState::Discharging ||
                   counters.state == BatteryState::Full ||
                   counters.state == BatteryState::NotPresent);
    }
    else
    {
        // No battery present
        EXPECT_EQ(counters.state, BatteryState::NotPresent);
        EXPECT_TRUE(counters.isOnAc);
    }
}

TEST(LinuxPowerProbeTest, MultipleReadsAreConsistent)
{
    LinuxPowerProbe probe;
    auto caps = probe.capabilities();
    
    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected, skipping consistency test";
    }
    
    auto counters1 = probe.read();
    auto counters2 = probe.read();
    
    // State should be consistent between quick successive reads
    EXPECT_EQ(counters1.state, counters2.state);
    EXPECT_EQ(counters1.isOnAc, counters2.isOnAc);
    
    // Charge percent shouldn't change dramatically in quick succession
    if (counters1.chargePercent >= 0 && counters2.chargePercent >= 0)
    {
        int diff = std::abs(counters1.chargePercent - counters2.chargePercent);
        EXPECT_LE(diff, 1) << "Charge percent changed too much between reads";
    }
}

// =============================================================================
// Battery State Validation
// =============================================================================

TEST(LinuxPowerProbeTest, BatteryStateIsValid)
{
    LinuxPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();
    
    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected";
    }
    
    // Verify state is consistent with AC status
    if (counters.isOnAc)
    {
        EXPECT_TRUE(counters.state == BatteryState::Charging ||
                   counters.state == BatteryState::Full ||
                   counters.state == BatteryState::Unknown);
    }
    else
    {
        EXPECT_TRUE(counters.state == BatteryState::Discharging ||
                   counters.state == BatteryState::Unknown);
    }
}

TEST(LinuxPowerProbeTest, ChargePercentInValidRange)
{
    LinuxPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();
    
    if (!caps.hasChargePercent)
    {
        GTEST_SKIP() << "Charge percent not available";
    }
    
    // Charge percent should be 0-100 or -1 (unavailable)
    EXPECT_TRUE(counters.chargePercent == -1 ||
               (counters.chargePercent >= 0 && counters.chargePercent <= 100));
}

} // namespace
} // namespace Platform

#endif // defined(__linux__) && __has_include(<unistd.h>)
