/// @file test_BatteryCapacity.cpp
/// @brief Tests for Platform/BatteryCapacity.h (#2409): the one rule both power probes combine a
/// battery's Design capacity, Full charge and Health by, so those rows and the Wear derived from them
/// never contradict each other.

#include "Platform/BatteryCapacity.h"
#include "Platform/Windows/WindowsPowerProbeMath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace Platform
{
namespace
{

TEST(BatteryCapacityRuleTest, HealthIsRoundedCappedAndUnknownWithoutBothValues)
{
    EXPECT_EQ(batteryHealthPercent(4.0, 4.5), 89);    // 88.9: rounded, not truncated
    EXPECT_EQ(batteryHealthPercent(80.0, 85.0), 94);  // 94.1
    EXPECT_EQ(batteryHealthPercent(52.0, 50.0), 100); // A new pack above its design: capped
    EXPECT_EQ(batteryHealthPercent(0.0, 50.0), -1);   // No full charge: unknown, not 0%
    EXPECT_EQ(batteryHealthPercent(45.0, 0.0), -1);   // No design: unknown, no division
}

TEST(BatteryCapacityRuleTest, EveryRowComesFromThePacksThatReportBoth)
{
    // The issue's case: a 50/45 Wh pack, a pack with no full charge and one with no design
    const std::array<PackCapacity, 3> packs{{
        {.fullCharge = 45.0, .design = 50.0},
        {.fullCharge = 0.0, .design = 30.0},
        {.fullCharge = 20.0, .design = 0.0},
    }};
    const CombinedCapacity combined = combinePackCapacities(packs);
    EXPECT_DOUBLE_EQ(combined.design, 50.0);
    EXPECT_DOUBLE_EQ(combined.fullCharge, 45.0);
    EXPECT_EQ(combined.healthPercent, 90);
}

TEST(BatteryCapacityRuleTest, HealthAndWearAreDerivableFromTheReportedSums)
{
    const std::array<PackCapacity, 3> packs{{
        {.fullCharge = 22.8, .design = 24.0},
        {.fullCharge = 43.2, .design = 72.0},
        {.fullCharge = 10.0, .design = 0.0}, // Left out of every row
    }};
    const CombinedCapacity combined = combinePackCapacities(packs);
    const double wear = std::max(0.0, (1.0 - (combined.fullCharge / combined.design)) * 100.0); // BatteryDetailsText's Wear
    EXPECT_EQ(combined.healthPercent, static_cast<int>(std::lround(100.0 - wear)));
    EXPECT_EQ(combined.healthPercent, batteryHealthPercent(combined.fullCharge, combined.design));
}

TEST(BatteryCapacityRuleTest, WithoutAPairedPackOnlyTheFullChargeIsKnown)
{
    const std::array<PackCapacity, 3> packs{{
        {.fullCharge = 20.0, .design = 0.0},
        {.fullCharge = 0.0, .design = 30.0}, // A design alone is never shown against another pack's full charge
        {.fullCharge = 25.0, .design = 0.0},
    }};
    const CombinedCapacity combined = combinePackCapacities(packs);
    EXPECT_DOUBLE_EQ(combined.fullCharge, 45.0);
    EXPECT_DOUBLE_EQ(combined.design, 0.0);
    EXPECT_EQ(combined.healthPercent, -1);

    const CombinedCapacity none = combinePackCapacities({});
    EXPECT_DOUBLE_EQ(none.fullCharge, 0.0);
    EXPECT_DOUBLE_EQ(none.design, 0.0);
    EXPECT_EQ(none.healthPercent, -1);
}

TEST(BatteryCapacityRuleTest, WindowsCombinesByTheSharedRule)
{
    // The same packs in mWh as Windows reads them, and in Wh as the shared rule takes them
    const std::array<BatteryCapacityReading, 3> readings{{
        {.designed = 50'000U, .fullCharged = 45'000U, .capabilities = 0U, .cycleCount = 0U},
        {.designed = 30'000U, .fullCharged = 0U, .capabilities = 0U, .cycleCount = 0U},
        {.designed = BATTERY_CAPACITY_UNKNOWN_VALUE, .fullCharged = 20'000U, .capabilities = 0U, .cycleCount = 0U},
    }};
    const std::array<PackCapacity, 3> packs{{
        {.fullCharge = 45.0, .design = 50.0},
        {.fullCharge = 0.0, .design = 30.0},
        {.fullCharge = 20.0, .design = 0.0},
    }};
    const BatteryCapacityTotals windows = combineBatteryCapacities(readings);
    const CombinedCapacity shared = combinePackCapacities(packs);
    EXPECT_DOUBLE_EQ(windows.designWh, shared.design);
    EXPECT_DOUBLE_EQ(windows.fullChargeWh, shared.fullCharge);
    EXPECT_EQ(windows.healthPercent, shared.healthPercent);
    EXPECT_EQ(healthPercentFromCapacity(4'500U, 4'000U, 0U), batteryHealthPercent(4.0, 4.5));
}

} // namespace
} // namespace Platform
