/// @file test_ProcessCellWidths.cpp
/// @brief Tests for App::ProcessCellWidths, the widths the Processes table's cell renderers read (#1382)

#include "App/Panels/ProcessesPanel.h"
#include "Domain/PriorityConfig.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

namespace
{

/// Widths 10, 20, 30, ... in PRIORITY_LABELS order, so a test can tell which one was returned.
App::ProcessCellWidths distinctWidths()
{
    App::ProcessCellWidths widths;
    for (std::size_t i = 0; i < widths.priorityLabels.size(); ++i)
    {
        widths.priorityLabels[i] = 10.0F * static_cast<float>(i + 1);
    }
    return widths;
}

TEST(ProcessCellWidthsTest, EachPriorityLabelGetsItsOwnWidth)
{
    const App::ProcessCellWidths widths = distinctWidths();
    for (std::size_t i = 0; i < App::PRIORITY_LABELS.size(); ++i)
    {
        SCOPED_TRACE(App::PRIORITY_LABELS[i]);
        EXPECT_FLOAT_EQ(widths.priorityLabelWidth(App::PRIORITY_LABELS[i]), widths.priorityLabels[i]);
    }
}

TEST(ProcessCellWidthsTest, EveryNiceValueMapsToAMeasuredLabel)
{
    // The Priority cell measures getPriorityLabel(nice) through this lookup, so every nice value must
    // land on one of the measured labels rather than the 0 fallback.
    const App::ProcessCellWidths widths = distinctWidths();
    for (int nice = Domain::Priority::MIN_NICE; nice <= Domain::Priority::MAX_NICE; ++nice)
    {
        SCOPED_TRACE(nice);
        EXPECT_GT(widths.priorityLabelWidth(Domain::Priority::getPriorityLabel(nice)), 0.0F);
    }
}

TEST(ProcessCellWidthsTest, EveryPriorityClassMapsToAMeasuredLabel)
{
    // On Windows the cell shows the class name (#1280), Realtime included.
    using Domain::Priority::PriorityClass;
    const App::ProcessCellWidths widths = distinctWidths();
    for (const PriorityClass priorityClass : {PriorityClass::Idle,
                                              PriorityClass::BelowNormal,
                                              PriorityClass::Normal,
                                              PriorityClass::AboveNormal,
                                              PriorityClass::High,
                                              PriorityClass::Realtime})
    {
        const std::string_view label = Domain::Priority::getProcessPriorityLabel(priorityClass, Domain::Priority::NORMAL_NICE);
        SCOPED_TRACE(label);
        EXPECT_GT(widths.priorityLabelWidth(label), 0.0F);
    }
}

TEST(ProcessCellWidthsTest, AnyOtherStringHasNoWidth)
{
    const App::ProcessCellWidths widths = distinctWidths();
    for (const std::string_view other : {"", "normal", "Real-time", "High "})
    {
        SCOPED_TRACE(other);
        EXPECT_FLOAT_EQ(widths.priorityLabelWidth(other), 0.0F);
    }
}

TEST(ProcessCellWidthsTest, DefaultsToUnmeasured)
{
    const App::ProcessCellWidths widths;
    EXPECT_FLOAT_EQ(widths.unitBytes, 0.0F);
    EXPECT_FLOAT_EQ(widths.unitBytesPerSec, 0.0F);
    EXPECT_FLOAT_EQ(widths.unitPower, 0.0F);
    EXPECT_FLOAT_EQ(widths.widestPriorityLabel, 0.0F);
    for (const float width : widths.priorityLabels)
    {
        EXPECT_FLOAT_EQ(width, 0.0F);
    }
}

} // namespace
