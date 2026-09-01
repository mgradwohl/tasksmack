#include "App/Panels/AdaptiveIntervalUtils.h"
#include "Domain/SamplingConfig.h"

#include <gtest/gtest.h>

#include <chrono>

namespace App
{
namespace
{

using std::chrono::milliseconds;

// =============================================================================
// chooseAdaptiveProcessInterval (ProcessesPanel's policy)
// =============================================================================

TEST(AdaptiveIntervalUtilsTest, ProcessActiveTabNoInteractionReturnsBaseUnchanged)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(500), true, false);
    EXPECT_EQ(result, milliseconds(500));
}

TEST(AdaptiveIntervalUtilsTest, ProcessBaseBelowMinimumIsFlooredToMinimum)
{
    // Below Domain::Sampling::REFRESH_INTERVAL_MIN_MS (100).
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(10), true, false);
    EXPECT_EQ(result, milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));
}

TEST(AdaptiveIntervalUtilsTest, ProcessInteractionTriplesBaseInterval)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(500), true, true);
    EXPECT_EQ(result, milliseconds(1500));
}

TEST(AdaptiveIntervalUtilsTest, ProcessInteractionClampsToMaximum)
{
    // 2000ms * 3 = 6000ms, above REFRESH_INTERVAL_MAX_MS (5000).
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(2000), true, true);
    EXPECT_EQ(result, milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
}

TEST(AdaptiveIntervalUtilsTest, ProcessInactiveTabDoublesBaseInterval)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(500), false, false);
    EXPECT_EQ(result, milliseconds(1000));
}

TEST(AdaptiveIntervalUtilsTest, ProcessInactiveTabClampsToMaximum)
{
    // 3000ms * 2 = 6000ms, above REFRESH_INTERVAL_MAX_MS (5000).
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(3000), false, false);
    EXPECT_EQ(result, milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
}

TEST(AdaptiveIntervalUtilsTest, ProcessInteractionTakesPrecedenceOverInactiveTab)
{
    // Both conditions true: interaction's 3x wins over inactive-tab's 2x.
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(milliseconds(500), false, true);
    EXPECT_EQ(result, milliseconds(1500));
}

// =============================================================================
// chooseAdaptiveSystemInterval (SystemMetricsPanel's policy)
// =============================================================================

TEST(AdaptiveIntervalUtilsTest, SystemActiveTabNoInteractionReturnsBaseUnchanged)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(500), true, false);
    EXPECT_EQ(result, milliseconds(500));
}

TEST(AdaptiveIntervalUtilsTest, SystemBaseBelowMinimumIsNotFloored)
{
    // Unlike chooseAdaptiveProcessInterval, the system variant does not enforce
    // REFRESH_INTERVAL_MIN_MS as a floor on the active-tab/no-interaction path.
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(10), true, false);
    EXPECT_EQ(result, milliseconds(10));
}

TEST(AdaptiveIntervalUtilsTest, SystemInteractionTriplesBaseInterval)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(500), true, true);
    EXPECT_EQ(result, milliseconds(1500));
}

TEST(AdaptiveIntervalUtilsTest, SystemInteractionClampsToMaximum)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(2000), true, true);
    EXPECT_EQ(result, milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
}

TEST(AdaptiveIntervalUtilsTest, SystemInactiveTabDoublesBaseInterval)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(500), false, false);
    EXPECT_EQ(result, milliseconds(1000));
}

TEST(AdaptiveIntervalUtilsTest, SystemInactiveTabClampsToMaximum)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(3000), false, false);
    EXPECT_EQ(result, milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
}

TEST(AdaptiveIntervalUtilsTest, SystemInteractionTakesPrecedenceOverInactiveTab)
{
    const auto result = AdaptiveIntervalUtils::chooseAdaptiveSystemInterval(milliseconds(500), false, true);
    EXPECT_EQ(result, milliseconds(1500));
}

} // namespace
} // namespace App
