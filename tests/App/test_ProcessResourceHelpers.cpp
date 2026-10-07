/// @file test_ProcessResourceHelpers.cpp
/// @brief Tests for the process details Resources chart's optional-series helpers: the GDI series'
/// time offset (#1001) and its missing samples (#1000).

#include "App/Panels/ProcessDetailsPanel_ResourceHelpers.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <optional>
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

TEST(ProcessResourceHelpersTest, MissingReadingMakesTheValueUnavailableAndTheNextStartsFresh)
{
    // valid -> missing -> valid: the missing sample doesn't ease the value toward 0, and the next
    // reading is taken as-is rather than smoothed from the stale value (#1148).
    SmoothedOptionalReading gdi = smoothOptionalReading({}, 100.0, 0.5, false);
    EXPECT_TRUE(gdi.available);
    EXPECT_DOUBLE_EQ(gdi.value, 100.0);

    gdi = smoothOptionalReading(gdi, 200.0, 0.5, true);
    EXPECT_TRUE(gdi.available);
    EXPECT_DOUBLE_EQ(gdi.value, 150.0); // smoothed halfway

    gdi = smoothOptionalReading(gdi, std::nullopt, 0.5, true);
    EXPECT_FALSE(gdi.available);
    EXPECT_DOUBLE_EQ(gdi.value, 150.0); // not eased toward 0

    gdi = smoothOptionalReading(gdi, 40.0, 0.5, true);
    EXPECT_TRUE(gdi.available);
    EXPECT_DOUBLE_EQ(gdi.value, 40.0); // starts fresh
}

TEST(ProcessResourceHelpersTest, GpuReadingAfterSupportIsGainedStartsFreshNotFromPlaceholderZeros)
{
    // #1210: while the GPU probe supplies no per-process utilization, samples are not readings (their
    // placeholder 0 is never fed in). The first real reading once support arrives is shown as-is
    // rather than easing up from those zeros.
    SmoothedOptionalReading gpuUtil;
    for (int i = 0; i < 3; ++i)
    {
        gpuUtil = smoothOptionalReading(gpuUtil, std::nullopt, 0.5, true);
        EXPECT_FALSE(gpuUtil.available);
    }
    gpuUtil = smoothOptionalReading(gpuUtil, 60.0, 0.5, true);
    EXPECT_TRUE(gpuUtil.available);
    EXPECT_DOUBLE_EQ(gpuUtil.value, 60.0);
}

TEST(ProcessResourceHelpersTest, OptionalReadingIsNeverNegative)
{
    EXPECT_DOUBLE_EQ(smoothOptionalReading({}, -5.0, 0.5, false).value, 0.0);
}

} // namespace
} // namespace App::Detail
