/// @file test_WindowsPowerProbe.cpp
/// @brief Integration tests for Platform::WindowsPowerProbe
///
/// These are integration tests that interact with the real Windows Power API.
/// They verify that the probe correctly reads and parses battery/power information.

#include <gtest/gtest.h>

#ifdef _WIN32

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsPowerProbe.h"
#include "Platform/Windows/WindowsPowerProbeMath.h"

#include <cstdint>

namespace Platform
{
namespace
{

// =============================================================================
// parsePowerStatus: pure parsing of SYSTEM_POWER_STATUS fields, no real battery
// required. CI runners have no battery, so these branches never execute against a
// real GetSystemPowerStatus() result without fabricating the field values here.
// =============================================================================

constexpr std::uint8_t BATTERY_FLAG_NO_BATTERY = 0x80;
constexpr std::uint8_t BATTERY_FLAG_UNKNOWN = 0xFF;
constexpr std::uint8_t BATTERY_FLAG_CHARGING = 0x08;
constexpr std::uint8_t BATTERY_FLAG_DISCHARGING = 0x00;
constexpr std::uint32_t BATTERY_LIFE_TIME_UNKNOWN = 0xFFFFFFFFU;

TEST(ParsePowerStatusTest, NoBatteryReportsNotPresentAndOnAc)
{
    const auto counters = parsePowerStatus(0, BATTERY_FLAG_NO_BATTERY, 0, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_TRUE(counters.isOnAc);
}

TEST(ParsePowerStatusTest, UnknownFlagReportsUnknownState)
{
    const auto counters = parsePowerStatus(1, BATTERY_FLAG_UNKNOWN, 50, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.state, BatteryState::Unknown);
    EXPECT_TRUE(counters.isOnAc);
}

TEST(ParsePowerStatusTest, ChargingFlagReportsCharging)
{
    const auto counters = parsePowerStatus(1, BATTERY_FLAG_CHARGING, 50, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.state, BatteryState::Charging);
}

TEST(ParsePowerStatusTest, FullChargeReportsFullRegardlessOfAc)
{
    const auto onBattery = parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 100, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(onBattery.state, BatteryState::Full);

    const auto onAc = parsePowerStatus(1, BATTERY_FLAG_DISCHARGING, 100, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(onAc.state, BatteryState::Full);
}

TEST(ParsePowerStatusTest, PartialChargeNotChargingReportsDischarging)
{
    const auto counters = parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 42, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 42);
}

TEST(ParsePowerStatusTest, OnAcLineStatusMapsToIsOnAc)
{
    EXPECT_TRUE(parsePowerStatus(1, BATTERY_FLAG_DISCHARGING, 50, BATTERY_LIFE_TIME_UNKNOWN).isOnAc);
    EXPECT_FALSE(parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 50, BATTERY_LIFE_TIME_UNKNOWN).isOnAc);
}

TEST(ParsePowerStatusTest, OutOfRangePercentReportsUnavailable)
{
    // 255 is BATTERY_FLAG_UNKNOWN's percent sentinel (unrelated to the flag byte).
    const auto counters = parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 255, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.chargePercent, -1);
}

TEST(ParsePowerStatusTest, UnknownTimeRemainingLeavesTimeToEmptyAtZero)
{
    const auto counters = parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 42, BATTERY_LIFE_TIME_UNKNOWN);
    EXPECT_EQ(counters.timeToEmptySec, 0U);
}

TEST(ParsePowerStatusTest, KnownTimeRemainingSetsTimeToEmptyWhenDischarging)
{
    const auto counters = parsePowerStatus(0, BATTERY_FLAG_DISCHARGING, 42, 3600U);
    EXPECT_EQ(counters.timeToEmptySec, 3600U);
}

TEST(ParsePowerStatusTest, KnownTimeRemainingIsIgnoredWhenNotDischarging)
{
    // Windows doesn't provide time-to-full for charging state.
    const auto counters = parsePowerStatus(1, BATTERY_FLAG_CHARGING, 42, 3600U);
    EXPECT_EQ(counters.state, BatteryState::Charging);
    EXPECT_EQ(counters.timeToEmptySec, 0U);
    EXPECT_EQ(counters.timeToFullSec, 0U);
}

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

    // Windows API has limited capabilities compared to Linux.
    // Charge percent is only reported when a battery exists, but the value may
    // still be unknown on some systems. The stable relationship is that
    // charge-percent capability implies a battery is present.
    EXPECT_FALSE(caps.hasChargePercent && !caps.hasBattery);

    EXPECT_FALSE(caps.hasChargeCapacity);
    EXPECT_FALSE(caps.hasPowerRate);
    EXPECT_FALSE(caps.hasVoltage);
    EXPECT_FALSE(caps.hasTechnology);
    EXPECT_FALSE(caps.hasCycleCount);
    EXPECT_FALSE(caps.hasHealthPercent);
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
        EXPECT_EQ(counters.chargePercent, -1);
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
