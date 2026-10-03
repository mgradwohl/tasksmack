/// @file test_ProcessResourceHelpers.cpp
/// @brief Tests for the process details Resources chart's optional-series helpers: the GDI series'
/// time offset (#1001) and its missing samples (#1000).

#include "App/Panels/ProcessDetailsPanel_ResourceHelpers.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <vector>

namespace App::Detail
{
namespace
{

constexpr double MISSING = std::numeric_limits<double>::quiet_NaN();

TEST(ProcessResourceHelpersTest, AShorterSeriesStartsAtTheLaterTimestamps)
{
    // Histories end at the same newest sample, so 3 GDI samples in a 5-sample chart cover
    // timestamps 2..4, not 0..2.
    EXPECT_EQ(seriesTimeOffset(5, 3), 2U);
    EXPECT_EQ(seriesTimeOffset(5, 5), 0U);
    EXPECT_EQ(seriesTimeOffset(5, 0), 5U);
    // A longer series is clamped to the chart, so it starts at the first timestamp.
    EXPECT_EQ(seriesTimeOffset(5, 8), 0U);
}

TEST(ProcessResourceHelpersTest, TheTooltipReadsTheSampleAtTheSameTimestamp)
{
    const std::vector<double> gdi{10.0, 20.0, 30.0};
    const std::size_t offset = seriesTimeOffset(5, gdi.size());

    EXPECT_FALSE(seriesValueAt(gdi, offset, 0).has_value());
    EXPECT_FALSE(seriesValueAt(gdi, offset, 1).has_value());
    EXPECT_DOUBLE_EQ(seriesValueAt(gdi, offset, 2).value_or(-1.0), 10.0);
    EXPECT_DOUBLE_EQ(seriesValueAt(gdi, offset, 4).value_or(-1.0), 30.0);
    EXPECT_FALSE(seriesValueAt(gdi, offset, 5).has_value());
}

TEST(ProcessResourceHelpersTest, AMissingSampleHasNoValue)
{
    // llround(NaN) used to print -9,223,372,036,854,775,808 in the tooltip.
    const std::vector<double> gdi{10.0, MISSING, 30.0};

    EXPECT_FALSE(seriesValueAt(gdi, 0, 1).has_value());
    EXPECT_DOUBLE_EQ(seriesValueAt(gdi, 0, 2).value_or(-1.0), 30.0);
}

TEST(ProcessResourceHelpersTest, ASeriesWithNoSampleIsNotDrawn)
{
    EXPECT_FALSE(hasAnySample(std::vector<double>{}));
    EXPECT_FALSE(hasAnySample(std::vector<double>{MISSING, MISSING}));
    EXPECT_TRUE(hasAnySample(std::vector<double>{MISSING, 0.0}));
}

} // namespace
} // namespace App::Detail
