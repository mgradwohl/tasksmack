/// @file test_WindowsPowerProbeMath.cpp
/// @brief Unit tests for WindowsPowerProbeMath.h's pure SYSTEM_POWER_STATUS parsing
///
/// WindowsPowerProbeMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests that need the real
/// probe stay in test_WindowsPowerProbe.cpp.

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsPowerProbeMath.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>

namespace Platform
{
namespace
{

// =============================================================================
// parsePowerStatus: pure parsing of SYSTEM_POWER_STATUS fields, no real battery
// required. CI runners have no battery, so these branches never execute against a
// real GetSystemPowerStatus() result without fabricating the field values here.
// =============================================================================

// winbase.h's BATTERY_FLAG_* / BATTERY_LIFE_TIME_UNKNOWN values, under names of their own so this
// file never collides with the SDK's macros (#1133).
constexpr std::uint8_t NO_BATTERY_FLAG = 0x80;
constexpr std::uint8_t UNKNOWN_FLAG = 0xFF;
constexpr std::uint8_t CHARGING_FLAG = 0x08;
constexpr std::uint8_t DISCHARGING_FLAG = 0x00;
constexpr std::uint32_t UNKNOWN_LIFE_TIME = 0xFFFFFFFFU;

// =============================================================================
// hasBatteryFromFlag: must stay consistent with parsePowerStatus()'s state parsing -
// hasBattery == false is only valid when the corresponding state is NotPresent (see
// PowerProbeContractTest.ReadReturnsSaneCounters).
// =============================================================================

TEST(HasBatteryFromFlagTest, NoBatteryBitReportsFalse)
{
    EXPECT_FALSE(hasBatteryFromFlag(NO_BATTERY_FLAG));
}

TEST(HasBatteryFromFlagTest, PresentBatteryFlagsReportTrue)
{
    EXPECT_TRUE(hasBatteryFromFlag(DISCHARGING_FLAG));
    EXPECT_TRUE(hasBatteryFromFlag(CHARGING_FLAG));
}

TEST(HasBatteryFromFlagTest, UnknownFlagReportsTrueNotFalse)
{
    // parsePowerStatus(BATTERY_FLAG_UNKNOWN) reports BatteryState::Unknown, not NotPresent,
    // so hasBattery must not be false here even though 0xFF also has the NO_BATTERY bit set -
    // otherwise hasBattery == false alongside a non-NotPresent state would violate the
    // power-probe contract.
    EXPECT_TRUE(hasBatteryFromFlag(UNKNOWN_FLAG));
}

TEST(ParsePowerStatusTest, NoBatteryReportsNotPresentAndOnAc)
{
    const auto counters = parsePowerStatus(0, NO_BATTERY_FLAG, 0, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.state, BatteryState::NotPresent);
    EXPECT_TRUE(counters.isOnAc);
}

TEST(ParsePowerStatusTest, UnknownFlagReportsUnknownState)
{
    const auto counters = parsePowerStatus(1, UNKNOWN_FLAG, 50, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.state, BatteryState::Unknown);
    EXPECT_TRUE(counters.isOnAc);
}

TEST(ParsePowerStatusTest, ChargingFlagReportsCharging)
{
    const auto counters = parsePowerStatus(1, CHARGING_FLAG, 50, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.state, BatteryState::Charging);
}

TEST(ParsePowerStatusTest, FullChargeReportsFullRegardlessOfAc)
{
    const auto onBattery = parsePowerStatus(0, DISCHARGING_FLAG, 100, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(onBattery.state, BatteryState::Full);

    const auto onAc = parsePowerStatus(1, DISCHARGING_FLAG, 100, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(onAc.state, BatteryState::Full);
}

TEST(ParsePowerStatusTest, OnAcBelowFullNotChargingReportsNotCharging)
{
    // Held below full by a charge limit while plugged in: NotCharging, as Linux reports it (#1158).
    const auto counters = parsePowerStatus(1, DISCHARGING_FLAG, 80, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.state, BatteryState::NotCharging);
    EXPECT_TRUE(counters.isOnAc);
    EXPECT_EQ(counters.chargePercent, 80);
}

TEST(ParsePowerStatusTest, PartialChargeNotChargingReportsDischarging)
{
    const auto counters = parsePowerStatus(0, DISCHARGING_FLAG, 42, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.state, BatteryState::Discharging);
    EXPECT_EQ(counters.chargePercent, 42);
}

TEST(ParsePowerStatusTest, OnAcLineStatusMapsToIsOnAc)
{
    EXPECT_TRUE(parsePowerStatus(1, DISCHARGING_FLAG, 50, UNKNOWN_LIFE_TIME).isOnAc);
    EXPECT_FALSE(parsePowerStatus(0, DISCHARGING_FLAG, 50, UNKNOWN_LIFE_TIME).isOnAc);
}

TEST(ParsePowerStatusTest, OutOfRangePercentReportsUnavailable)
{
    // 255 is BATTERY_FLAG_UNKNOWN's percent sentinel (unrelated to the flag byte).
    const auto counters = parsePowerStatus(0, DISCHARGING_FLAG, 255, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.chargePercent, -1);
}

TEST(ParsePowerStatusTest, UnknownTimeRemainingLeavesTimeToEmptyAtZero)
{
    const auto counters = parsePowerStatus(0, DISCHARGING_FLAG, 42, UNKNOWN_LIFE_TIME);
    EXPECT_EQ(counters.timeToEmptySec, 0U);
}

TEST(ParsePowerStatusTest, KnownTimeRemainingSetsTimeToEmptyWhenDischarging)
{
    const auto counters = parsePowerStatus(0, DISCHARGING_FLAG, 42, 3600U);
    EXPECT_EQ(counters.timeToEmptySec, 3600U);
}

TEST(ParsePowerStatusTest, KnownTimeRemainingIsIgnoredWhenNotDischarging)
{
    // Windows doesn't provide time-to-full for charging state.
    const auto counters = parsePowerStatus(1, CHARGING_FLAG, 42, 3600U);
    EXPECT_EQ(counters.state, BatteryState::Charging);
    EXPECT_EQ(counters.timeToEmptySec, 0U);
    EXPECT_EQ(counters.timeToFullSec, 0U);
}

// ---- Battery details (#1523) ----

TEST(BatteryChemistryTest, KnownCodesReadAsLinuxTechnologyNames)
{
    EXPECT_EQ(chemistryText("LION"), "Li-ion");
    EXPECT_EQ(chemistryText("LiOn"), "Li-ion"); // Case-insensitive: drivers vary
    EXPECT_EQ(chemistryText("Li-I"), "Li-ion");
    EXPECT_EQ(chemistryText(std::string_view("LiP\0", 4)), "Li-poly"); // A three-letter code, NUL-padded
    EXPECT_EQ(chemistryText("PbAc"), "Lead-acid");
    EXPECT_EQ(chemistryText("NiCd"), "NiCd");
    EXPECT_EQ(chemistryText("NiMH"), "NiMH");
    EXPECT_EQ(chemistryText("NiZn"), "NiZn");
    EXPECT_EQ(chemistryText("RAM "), "Alkaline-manganese"); // Space-padded
}

TEST(BatteryChemistryTest, UnlistedCodeIsShownAsReportedAndGarbageIsUnknown)
{
    EXPECT_EQ(chemistryText("LiFe"), "LiFe");
    EXPECT_EQ(chemistryText(std::string_view("\0\0\0\0", 4)), "");
    EXPECT_EQ(chemistryText("    "), "");
    EXPECT_EQ(chemistryText("\x01\x02"), "");
}

TEST(BatteryCapacityTest, MilliwattHoursAreWattHoursUnlessUnknownOrRelative)
{
    EXPECT_DOUBLE_EQ(capacityWh(52'600U, 0U), 52.6);
    EXPECT_DOUBLE_EQ(capacityWh(0U, 0U), 0.0);
    EXPECT_DOUBLE_EQ(capacityWh(BATTERY_CAPACITY_UNKNOWN_VALUE, 0U), 0.0);
    EXPECT_DOUBLE_EQ(capacityWh(100U, BATTERY_CAPACITY_RELATIVE_BIT), 0.0);
}

TEST(BatteryCapacityTest, HealthIsFullOverDesignRoundedAndCapped)
{
    EXPECT_EQ(healthPercentFromCapacity(50'000U, 45'000U, 0U), 90);
    EXPECT_EQ(healthPercentFromCapacity(50'000U, 52'000U, 0U), 100); // Above design: capped
    EXPECT_EQ(healthPercentFromCapacity(0U, 45'000U, 0U), -1);       // No design: unknown, no division
    EXPECT_EQ(healthPercentFromCapacity(50'000U, 0U, 0U), -1);
    EXPECT_EQ(healthPercentFromCapacity(100U, 90U, BATTERY_CAPACITY_RELATIVE_BIT), -1); // Relative units
}

} // namespace
} // namespace Platform
