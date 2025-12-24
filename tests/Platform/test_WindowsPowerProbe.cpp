/// @file test_WindowsPowerProbe.cpp
/// @brief Integration tests for Platform::WindowsPowerProbe
///
/// These are integration tests that interact with the real Windows Power API.
/// They verify that the probe correctly reads and parses battery/power information.

#include <gtest/gtest.h>

#ifdef _WIN32

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsPowerProbe.h"

namespace Platform
{
namespace
{

// =============================================================================
// Construction and Basic Operations
// =============================================================================

TEST(WindowsPowerProbeTest, ConstructsSuccessfully)
{
    EXPECT_NO_THROW({ WindowsPowerProbe probe; });
}

TEST(WindowsPowerProbeTest, CapabilitiesReportedCorrectly)
{
    WindowsPowerProbe probe;
    auto caps = probe.capabilities();

    // Windows API has limited capabilities compared to Linux
    // If there's a battery, charge percent should be available
    if (caps.hasBattery)
    {
        EXPECT_TRUE(caps.hasChargePercent);
    }
}

TEST(WindowsPowerProbeTest, ReadSucceeds)
{
    WindowsPowerProbe probe;
    PowerCounters counters;

    EXPECT_NO_THROW({ counters = probe.read(); });

    // Verify basic structure validity
    auto caps = probe.capabilities();
    if (caps.hasBattery)
    {
        // If battery is present, charge percent should be valid range or -1
        EXPECT_TRUE(counters.chargePercent == -1 || (counters.chargePercent >= 0 && counters.chargePercent <= 100));

        // Battery state should be one of the valid states
        EXPECT_TRUE(counters.state == BatteryState::Unknown || counters.state == BatteryState::Charging ||
                    counters.state == BatteryState::Discharging || counters.state == BatteryState::Full ||
                    counters.state == BatteryState::NotPresent);
    }
    else
    {
        // No battery present
        EXPECT_EQ(counters.state, BatteryState::NotPresent);
        EXPECT_TRUE(counters.isOnAc);
    }
}

TEST(WindowsPowerProbeTest, MultipleReadsAreConsistent)
{
    WindowsPowerProbe probe;
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

TEST(WindowsPowerProbeTest, BatteryStateIsValid)
{
    WindowsPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();

    if (!caps.hasBattery)
    {
        GTEST_SKIP() << "No battery detected";
    }

    // Verify state is consistent with AC status
    if (counters.isOnAc && counters.chargePercent == 100)
    {
        EXPECT_TRUE(counters.state == BatteryState::Charging || counters.state == BatteryState::Full ||
                    counters.state == BatteryState::Unknown);
    }
}

TEST(WindowsPowerProbeTest, ChargePercentInValidRange)
{
    WindowsPowerProbe probe;
    auto counters = probe.read();
    auto caps = probe.capabilities();

    if (!caps.hasChargePercent)
    {
        GTEST_SKIP() << "Charge percent not available";
    }

    // Charge percent should be 0-100 or -1 (unavailable)
    EXPECT_TRUE(counters.chargePercent == -1 || (counters.chargePercent >= 0 && counters.chargePercent <= 100));
}

} // namespace
} // namespace Platform

#endif // _WIN32
