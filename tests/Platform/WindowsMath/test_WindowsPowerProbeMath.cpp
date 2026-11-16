/// @file test_WindowsPowerProbeMath.cpp
/// @brief Unit tests for WindowsPowerProbeMath.h's pure SYSTEM_POWER_STATUS parsing
///
/// WindowsPowerProbeMath.h includes no Windows header, so these tests build and run on every
/// platform, including Linux CI's sanitizer and coverage jobs (#1133). Tests that need the real
/// probe stay in test_WindowsPowerProbe.cpp.

#include "Platform/PowerTypes.h"
#include "Platform/Windows/WindowsPowerProbeMath.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <new>
#include <span>
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

// =============================================================================
// powerCapabilitiesFrom: recomputed on every read (#2413)
// =============================================================================

constexpr BatteryDetailsReported FULL_DETAILS{
    .found = true,
    .designWh = 52.6,
    .fullChargeWh = 47.34,
    .healthPercent = 90,
    .cycleCount = 123,
    .hasTechnology = true,
};

TEST(PowerCapabilitiesFromTest, ABatteryWithItsDetailsOffersEachOne)
{
    const PowerCapabilities caps = powerCapabilitiesFrom(DISCHARGING_FLAG, 80, 3'600, FULL_DETAILS);
    EXPECT_TRUE(caps.hasBattery);
    EXPECT_TRUE(caps.hasChargePercent);
    EXPECT_TRUE(caps.hasTimeEstimates);
    EXPECT_TRUE(caps.hasDesignCapacity);
    EXPECT_TRUE(caps.hasHealthPercent);
    EXPECT_TRUE(caps.hasCycleCount);
    EXPECT_TRUE(caps.hasTechnology);
    // GetSystemPowerStatus has no live capacity, rate or voltage; only the battery's status gives them
    EXPECT_FALSE(caps.hasChargeCapacity || caps.hasPowerRate || caps.hasVoltage);
}

TEST(PowerCapabilitiesFromTest, TheBatteryStatusOffersTheLiveFiguresAndATimeToFull)
{
    // #4030: charging, so GetSystemPowerStatus has no time left; the rate and remaining Wh give a time to full
    BatteryDetailsReported details = FULL_DETAILS;
    details.hasPowerRate = true;
    details.hasChargeCapacity = true;
    details.hasVoltage = true;
    const PowerCapabilities caps = powerCapabilitiesFrom(CHARGING_FLAG, 60, UNKNOWN_LIFE_TIME, details);
    EXPECT_TRUE(caps.hasPowerRate);
    EXPECT_TRUE(caps.hasChargeCapacity);
    EXPECT_TRUE(caps.hasVoltage);
    EXPECT_TRUE(caps.hasTimeEstimates);

    details.hasChargeCapacity = false; // No remaining Wh: no time to full
    EXPECT_FALSE(powerCapabilitiesFrom(CHARGING_FLAG, 60, UNKNOWN_LIFE_TIME, details).hasTimeEstimates);
    details.hasChargeCapacity = true;
    EXPECT_FALSE(powerCapabilitiesFrom(NO_BATTERY_FLAG, 255, UNKNOWN_LIFE_TIME, details).hasPowerRate) << "no battery, nothing";
}

TEST(PowerCapabilitiesFromTest, NoBatteryOffersNothingWhateverTheDeviceLastSaid)
{
    // NotPresent is no battery (#2408), even with details left from a battery since removed
    const PowerCapabilities caps = powerCapabilitiesFrom(NO_BATTERY_FLAG, 255, UNKNOWN_LIFE_TIME, FULL_DETAILS);
    EXPECT_FALSE(caps.hasBattery);
    EXPECT_FALSE(caps.hasChargePercent || caps.hasTimeEstimates);
    EXPECT_FALSE(caps.hasDesignCapacity || caps.hasHealthPercent || caps.hasCycleCount || caps.hasTechnology);
}

TEST(PowerCapabilitiesFromTest, DetailsTheDeviceDidNotGiveAreNotOffered)
{
    const PowerCapabilities unanswered = powerCapabilitiesFrom(CHARGING_FLAG, 255, UNKNOWN_LIFE_TIME, BatteryDetailsReported{});
    EXPECT_TRUE(unanswered.hasBattery);
    EXPECT_FALSE(unanswered.hasChargePercent || unanswered.hasTimeEstimates);
    EXPECT_FALSE(unanswered.hasDesignCapacity || unanswered.hasHealthPercent || unanswered.hasCycleCount || unanswered.hasTechnology);

    BatteryDetailsReported relative = FULL_DETAILS; // Relative units, no cycle counter
    relative.designWh = 0.0;
    relative.fullChargeWh = 0.0;
    relative.healthPercent = -1;
    relative.cycleCount = 0;
    const PowerCapabilities caps = powerCapabilitiesFrom(UNKNOWN_FLAG, 50, UNKNOWN_LIFE_TIME, relative);
    EXPECT_TRUE(caps.hasBattery); // BATTERY_FLAG_UNKNOWN is not "no battery"
    EXPECT_FALSE(caps.hasDesignCapacity || caps.hasHealthPercent || caps.hasCycleCount);
    EXPECT_TRUE(caps.hasTechnology);
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

TEST(BatteryCapacityTest, OnePacksHealthIsFullOverDesignRoundedAndCapped)
{
    const auto health = [](std::uint32_t designed, std::uint32_t fullCharged, std::uint32_t capabilities)
    {
        const std::array<BatteryCapacityReading, 1> pack{
            {{.designed = designed, .fullCharged = fullCharged, .capabilities = capabilities, .cycleCount = 0U}},
        };
        return combineBatteryCapacities(pack).healthPercent;
    };
    EXPECT_EQ(health(50'000U, 45'000U, 0U), 90);
    EXPECT_EQ(health(4'500U, 4'000U, 0U), 89);    // 88.9, rounded as on Linux (#2409)
    EXPECT_EQ(health(50'000U, 52'000U, 0U), 100); // Above design: capped
    EXPECT_EQ(health(0U, 45'000U, 0U), -1);       // No design: unknown, no division
    EXPECT_EQ(health(50'000U, 0U, 0U), -1);
    EXPECT_EQ(health(100U, 90U, BATTERY_CAPACITY_RELATIVE_BIT), -1); // Relative units
}

TEST(BatteryCapacityTest, TwoBatteriesAreSummedAndHealthComesFromTheSums)
{
    // #2151: a 24 Wh internal pack at 95% and a 72 Wh external pack at 60%
    const std::array<BatteryCapacityReading, 2> packs{
        {
            {.designed = 24'000U, .fullCharged = 22'800U, .capabilities = 0U, .cycleCount = 40U},
            {.designed = 72'000U, .fullCharged = 43'200U, .capabilities = 0U, .cycleCount = 310U},
        },
    };
    const BatteryCapacityTotals totals = combineBatteryCapacities(packs);
    EXPECT_FALSE(totals.relativeCapacity);
    EXPECT_DOUBLE_EQ(totals.designWh, 96.0);
    EXPECT_DOUBLE_EQ(totals.fullChargeWh, 66.0);
    EXPECT_EQ(totals.healthPercent, 69); // 66 / 96 = 68.75%, not the first pack's 95%
    EXPECT_EQ(totals.cycleCount, 310U);  // The most-cycled pack
}

TEST(BatteryCapacityTest, RelativeAndUnknownCapacitiesAreLeftOutOfTheTotals)
{
    const std::array<BatteryCapacityReading, 3> packs{
        {
            {.designed = 50'000U, .fullCharged = 45'000U, .capabilities = 0U, .cycleCount = 0U},
            {.designed = 100U, .fullCharged = 80U, .capabilities = BATTERY_CAPACITY_RELATIVE_BIT, .cycleCount = 12U},
            {.designed = 30'000U, .fullCharged = BATTERY_CAPACITY_UNKNOWN_VALUE, .capabilities = 0U, .cycleCount = 0U},
        },
    };
    const BatteryCapacityTotals totals = combineBatteryCapacities(packs);
    EXPECT_FALSE(totals.relativeCapacity);
    // Design, full charge and health all describe the one pack that reports both (#2409), not
    // Design 80 Wh and Wear 43.8% next to Health 90%
    EXPECT_DOUBLE_EQ(totals.designWh, 50.0);
    EXPECT_DOUBLE_EQ(totals.fullChargeWh, 45.0);
    EXPECT_EQ(totals.healthPercent, 90);
    EXPECT_EQ(totals.cycleCount, 12U);
}

TEST(BatteryCapacityTest, APackWithoutADesignCapacityIsLeftOutOfEveryRow)
{
    // #2409: the second pack's full charge would otherwise be weighed against the first's design
    const std::array<BatteryCapacityReading, 2> packs{
        {
            {.designed = 50'000U, .fullCharged = 40'000U, .capabilities = 0U, .cycleCount = 0U},
            {.designed = 0U, .fullCharged = 30'000U, .capabilities = 0U, .cycleCount = 0U},
        },
    };
    const BatteryCapacityTotals totals = combineBatteryCapacities(packs);
    EXPECT_DOUBLE_EQ(totals.designWh, 50.0);
    EXPECT_DOUBLE_EQ(totals.fullChargeWh, 40.0);
    EXPECT_EQ(totals.healthPercent, 80);
}

TEST(BatteryCapacityTest, WithoutAnyDesignCapacityOnlyTheFullChargeIsReported)
{
    const std::array<BatteryCapacityReading, 2> packs{
        {
            {.designed = BATTERY_CAPACITY_UNKNOWN_VALUE, .fullCharged = 40'000U, .capabilities = 0U, .cycleCount = 0U},
            {.designed = 0U, .fullCharged = 30'000U, .capabilities = 0U, .cycleCount = 0U},
        },
    };
    const BatteryCapacityTotals totals = combineBatteryCapacities(packs);
    EXPECT_DOUBLE_EQ(totals.designWh, 0.0);
    EXPECT_DOUBLE_EQ(totals.fullChargeWh, 70.0);
    EXPECT_EQ(totals.healthPercent, -1);
}

TEST(BatteryCapacityTest, NoBatteriesOrOnlyRelativeOnesReportNoCapacity)
{
    const BatteryCapacityTotals none = combineBatteryCapacities({});
    EXPECT_FALSE(none.relativeCapacity);
    EXPECT_DOUBLE_EQ(none.designWh, 0.0);
    EXPECT_EQ(none.healthPercent, -1);

    const std::array<BatteryCapacityReading, 2> relative{
        {
            {.designed = 100U, .fullCharged = 92U, .capabilities = BATTERY_CAPACITY_RELATIVE_BIT, .cycleCount = 5U},
            {.designed = 100U, .fullCharged = 70U, .capabilities = BATTERY_CAPACITY_RELATIVE_BIT, .cycleCount = 9U},
        },
    };
    const BatteryCapacityTotals totals = combineBatteryCapacities(relative);
    EXPECT_TRUE(totals.relativeCapacity);
    EXPECT_DOUBLE_EQ(totals.designWh, 0.0);
    EXPECT_DOUBLE_EQ(totals.fullChargeWh, 0.0);
    EXPECT_EQ(totals.healthPercent, -1);
    EXPECT_EQ(totals.cycleCount, 9U);
}

// =============================================================================
// combineBatteryStatus, timeToFullSeconds: IOCTL_BATTERY_QUERY_STATUS (#4030)
// =============================================================================

TEST(BatteryStatusTest, DischargingIsPositiveWattsAndChargingNegativeAsOnLinux)
{
    const std::array discharging{BatteryStatusReading{.rate = -12'500, .capacity = 37'872, .voltage = 11'400, .relative = false}};
    const BatteryStatusTotals out = combineBatteryStatus(discharging);
    EXPECT_TRUE(out.hasPowerRate);
    EXPECT_DOUBLE_EQ(out.powerNowW, 12.5);
    EXPECT_TRUE(out.hasChargeCapacity);
    EXPECT_DOUBLE_EQ(out.chargeNowWh, 37.872);
    EXPECT_TRUE(out.hasVoltage);
    EXPECT_EQ(out.voltageMv, 11'400U);

    const std::array charging{BatteryStatusReading{.rate = 20'000, .capacity = 30'000, .voltage = 12'600, .relative = false}};
    EXPECT_DOUBLE_EQ(combineBatteryStatus(charging).powerNowW, -20.0);

    const std::array idle{BatteryStatusReading{.rate = 0, .capacity = 50'000, .voltage = 12'900, .relative = false}};
    const BatteryStatusTotals full = combineBatteryStatus(idle);
    EXPECT_TRUE(full.hasPowerRate) << "0 W is a known rate";
    EXPECT_DOUBLE_EQ(full.powerNowW, 0.0);
}

TEST(BatteryStatusTest, UnknownValuesAreNotOffered)
{
    const std::array unknown{BatteryStatusReading{}};
    const BatteryStatusTotals out = combineBatteryStatus(unknown);
    EXPECT_FALSE(out.hasPowerRate);
    EXPECT_FALSE(out.hasChargeCapacity);
    EXPECT_FALSE(out.hasVoltage);
    EXPECT_DOUBLE_EQ(out.powerNowW, 0.0);
    EXPECT_DOUBLE_EQ(out.chargeNowWh, 0.0);
    EXPECT_EQ(out.voltageMv, 0U);

    const std::array zeroVolts{BatteryStatusReading{.rate = -5'000, .capacity = 10'000, .voltage = 0, .relative = false}};
    EXPECT_FALSE(combineBatteryStatus(zeroVolts).hasVoltage);
    EXPECT_FALSE(combineBatteryStatus(std::span<const BatteryStatusReading>{}).hasPowerRate);
}

TEST(BatteryStatusTest, TwoBatteriesAreSummedAndTheVoltageIsTheFirstOnes)
{
    const std::array two{
        BatteryStatusReading{.rate = -12'500, .capacity = 37'872, .voltage = 11'400, .relative = false},
        BatteryStatusReading{.rate = 3'000, .capacity = 20'000, .voltage = 12'000, .relative = false},
    };
    const BatteryStatusTotals out = combineBatteryStatus(two);
    EXPECT_DOUBLE_EQ(out.powerNowW, 9.5) << "one pack tops the other up: the net rate";
    EXPECT_DOUBLE_EQ(out.chargeNowWh, 57.872);
    EXPECT_EQ(out.voltageMv, 11'400U);
}

TEST(BatteryStatusTest, APackWithAnUnknownOrRelativeChargeLeavesNoSumButItsRateCounts)
{
    // As Linux (#2409): a charge sum missing a pack would read as the whole battery's
    const std::array unknownCharge{
        BatteryStatusReading{.rate = -10'000, .capacity = 30'000, .voltage = 11'400, .relative = false},
        BatteryStatusReading{.rate = -2'000, .capacity = BATTERY_CAPACITY_UNKNOWN_VALUE, .voltage = 12'000, .relative = false},
    };
    const BatteryStatusTotals out = combineBatteryStatus(unknownCharge);
    EXPECT_FALSE(out.hasChargeCapacity);
    EXPECT_DOUBLE_EQ(out.chargeNowWh, 0.0);
    EXPECT_DOUBLE_EQ(out.powerNowW, 12.0);

    const std::array relative{
        BatteryStatusReading{.rate = -10'000, .capacity = 30'000, .voltage = 11'400, .relative = false},
        BatteryStatusReading{.rate = -40, .capacity = 80, .voltage = 12'000, .relative = true},
    };
    const BatteryStatusTotals mixed = combineBatteryStatus(relative);
    EXPECT_FALSE(mixed.hasChargeCapacity);
    EXPECT_DOUBLE_EQ(mixed.powerNowW, 10.0) << "the relative pack's rate isn't watts";

    const std::array onlyRelative{BatteryStatusReading{.rate = -40, .capacity = 80, .voltage = 12'000, .relative = true}};
    const BatteryStatusTotals none = combineBatteryStatus(onlyRelative);
    EXPECT_FALSE(none.hasPowerRate);
    EXPECT_FALSE(none.hasChargeCapacity);
    EXPECT_TRUE(none.hasVoltage) << "volts are volts";
}

TEST(BatteryStatusTest, TimeToFullComesFromTheChargingRateAsOnLinux)
{
    EXPECT_EQ(timeToFullSeconds(30.0, 50.0, -20.0), 3'600U);
    EXPECT_EQ(timeToFullSeconds(30.0, 50.0, 20.0), 0U) << "discharging";
    EXPECT_EQ(timeToFullSeconds(30.0, 50.0, -0.05), 0U) << "a trickle gives no estimate";
    EXPECT_EQ(timeToFullSeconds(50.0, 50.0, -20.0), 0U) << "already full";
}

// =============================================================================
// takeNewerPublication: WindowsPowerProbe's take of the battery worker's publication (#3343). A copy
// that throws (no memory for the battery's strings) must leave the publication to be taken again: marking
// it taken first stalled the facts for good, every later re-read deferred behind it.
// =============================================================================

/// A snapshot whose copy throws while told to, as BatteryInfo's strings do when memory runs out.
struct CopyMayThrow
{
    static inline bool throwOnCopy = false;
    int facts = 0;

    CopyMayThrow() = default;
    explicit CopyMayThrow(int value) : facts(value)
    {}
    CopyMayThrow(const CopyMayThrow& other) : facts(other.facts)
    {
        if (throwOnCopy)
        {
            throw std::bad_alloc();
        }
    }
    CopyMayThrow& operator=(const CopyMayThrow& other) = delete;
    CopyMayThrow(CopyMayThrow&&) noexcept = default;
    CopyMayThrow& operator=(CopyMayThrow&&) noexcept = default;
    ~CopyMayThrow() = default;
};

/// Domain::WorkerPublication's shape: a version and a value.
struct FakePublication
{
    std::uint64_t version = 0;
    CopyMayThrow value;
};

/// Resets CopyMayThrow::throwOnCopy when the test leaves, failed or not.
class CopyThrowReset
{
  public:
    CopyThrowReset() = default;
    ~CopyThrowReset()
    {
        CopyMayThrow::throwOnCopy = false;
    }
    CopyThrowReset(const CopyThrowReset&) = delete;
    CopyThrowReset& operator=(const CopyThrowReset&) = delete;
    CopyThrowReset(CopyThrowReset&&) = delete;
    CopyThrowReset& operator=(CopyThrowReset&&) = delete;
};

TEST(TakeNewerPublicationTest, TakesANewerOneOnce)
{
    const FakePublication published{.version = 2, .value = CopyMayThrow(7)};
    CopyMayThrow facts(1);
    std::uint64_t taken = 1;
    EXPECT_TRUE(takeNewerPublication(published, facts, taken));
    EXPECT_EQ(facts.facts, 7);
    EXPECT_EQ(taken, 2U);
    EXPECT_FALSE(takeNewerPublication(published, facts, taken)) << "taken already";
    EXPECT_EQ(facts.facts, 7);
}

TEST(TakeNewerPublicationTest, WhoseCopyThrowsChangesNothingAndIsTakenNextTime)
{
    const CopyThrowReset reset;
    const FakePublication published{.version = 2, .value = CopyMayThrow(7)};
    CopyMayThrow facts(1);
    std::uint64_t taken = 1;

    CopyMayThrow::throwOnCopy = true;
    EXPECT_THROW(static_cast<void>(takeNewerPublication(published, facts, taken)), std::bad_alloc);
    EXPECT_EQ(facts.facts, 1) << "the facts known so far are kept";
    EXPECT_EQ(taken, 1U) << "not marked taken: the next read() takes it again";

    CopyMayThrow::throwOnCopy = false;
    EXPECT_TRUE(takeNewerPublication(published, facts, taken)) << "the retry takes it, rather than stalling on it";
    EXPECT_EQ(facts.facts, 7);
    EXPECT_EQ(taken, 2U);
}

} // namespace
} // namespace Platform
