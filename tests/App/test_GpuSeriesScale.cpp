/// @file test_GpuSeriesScale.cpp
/// @brief Tests for the scales the GPU clock, temperature and power lines are drawn as a percentage
/// of, and the series labels that name them (#1205).

#include "App/Panels/GpuSection.h"
#include "UI/Format.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace App
{
namespace
{

using GpuSection::GPU_ASSUMED_POWER_LIMIT_WATTS;
using GpuSection::GPU_CLOCK_REFERENCE_FLOOR_MHZ;
using GpuSection::GPU_TEMPERATURE_SCALE_C;
using GpuSection::gpuClockScaleMHz;
using GpuSection::gpuClockSeriesLabel;
using GpuSection::GpuPowerScale;
using GpuSection::gpuPowerScale;
using GpuSection::gpuPowerSeriesLabel;
using GpuSection::GpuScaledLabels;
using GpuSection::gpuTemperatureSeriesLabel;

TEST(GpuSeriesScaleTest, ClockScaleRoundsTheReferenceUpToAStep)
{
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(2000.0F), 2000.0F);
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(2001.0F), 2100.0F);
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(2602.0F), 2700.0F);
}

TEST(GpuSeriesScaleTest, ClockScaleIsNeverBelowTheReference)
{
    // So no sample, and not the bar's smoothed clock, is drawn above 100 %.
    for (const float reference : {2000.0F, 2050.5F, 2099.9F, 2100.0F, 3333.0F})
    {
        EXPECT_GE(gpuClockScaleMHz(reference), reference);
    }
}

TEST(GpuSeriesScaleTest, ClockScaleOfANonsensicalReferenceIsTheFloor)
{
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(0.0F), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(-5.0F), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
    EXPECT_FLOAT_EQ(gpuClockScaleMHz(std::numeric_limits<float>::quiet_NaN()), GPU_CLOCK_REFERENCE_FLOOR_MHZ);
}

TEST(GpuSeriesScaleTest, PowerScaleIsTheReportedLimit)
{
    const GpuPowerScale scale = gpuPowerScale(250.0);
    EXPECT_DOUBLE_EQ(scale.watts, 250.0);
    EXPECT_FALSE(scale.assumed);
}

TEST(GpuSeriesScaleTest, PowerScaleWithoutALimitIsAssumed)
{
    for (const double limit : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
    {
        const GpuPowerScale scale = gpuPowerScale(limit);
        EXPECT_DOUBLE_EQ(scale.watts, GPU_ASSUMED_POWER_LIMIT_WATTS);
        EXPECT_TRUE(scale.assumed);
    }
}

// The labels name what 100 % of the percent axis is, in the unit the series' values are shown in.
TEST(GpuSeriesScaleTest, LabelsNameTheirScale)
{
    EXPECT_EQ(gpuClockSeriesLabel(2100.0F), "Clock (% of 2100 MHz)");
    EXPECT_EQ(gpuTemperatureSeriesLabel(), "Temperature (% of " + UI::Format::formatCelsius(GPU_TEMPERATURE_SCALE_C) + ")");
    EXPECT_EQ(gpuPowerSeriesLabel(gpuPowerScale(250.0)), "Power (% of " + UI::Format::formatWatts(250.0) + " limit)");
}

TEST(GpuSeriesScaleTest, AnAssumedPowerScaleSaysSo)
{
    const std::string label = gpuPowerSeriesLabel(gpuPowerScale(0.0));
    EXPECT_EQ(label, "Power (% of " + UI::Format::formatWatts(GPU_ASSUMED_POWER_LIMIT_WATTS) + ", assumed)");
    EXPECT_NE(label.find("assumed"), std::string::npos);
}

TEST(GpuSeriesScaleTest, ScaledLabelsAreBuiltOnFirstUseAndFollowTheirScale)
{
    GpuScaledLabels labels;
    labels.update(2000.0F, gpuPowerScale(0.0));
    EXPECT_EQ(labels.clock, gpuClockSeriesLabel(2000.0F));
    EXPECT_EQ(labels.power, gpuPowerSeriesLabel(gpuPowerScale(0.0)));

    labels.update(2100.0F, gpuPowerScale(250.0));
    EXPECT_EQ(labels.clock, gpuClockSeriesLabel(2100.0F));
    EXPECT_EQ(labels.power, gpuPowerSeriesLabel(gpuPowerScale(250.0)));
}

TEST(GpuSeriesScaleTest, ScaledLabelsAreNotRebuiltWhileTheirScaleHolds)
{
    GpuScaledLabels labels;
    labels.update(2000.0F, gpuPowerScale(250.0));
    // The same buffers, not rebuilt strings: shown every frame, they must not allocate (#1171).
    const char* const clock = labels.clock.data();
    const char* const power = labels.power.data();
    labels.update(2000.0F, gpuPowerScale(250.0));
    EXPECT_EQ(labels.clock.data(), clock);
    EXPECT_EQ(labels.power.data(), power);
}

} // namespace
} // namespace App
