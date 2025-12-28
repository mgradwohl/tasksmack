#include "App/ProcessColumnConfig.h"

#include <gtest/gtest.h>

#include <cstddef>

namespace App
{
namespace
{

// ========== Column Count and Index Conversion ==========

TEST(ProcessColumnConfigTest, ColumnCountIsCorrect)
{
    constexpr auto count = processColumnCount();
    constexpr auto expected = static_cast<std::size_t>(ProcessColumn::Count);
    EXPECT_EQ(count, expected);
}

TEST(ProcessColumnConfigTest, AllColumnsArraySizeMatchesCount)
{
    constexpr auto columns = allProcessColumns();
    constexpr auto count = processColumnCount();
    EXPECT_EQ(columns.size(), count);
}

TEST(ProcessColumnConfigTest, ToIndexReturnsCorrectValues)
{
    // Test known columns (indices based on ProcessColumn enum definition)
    EXPECT_EQ(toIndex(ProcessColumn::PID), 0);
    EXPECT_EQ(toIndex(ProcessColumn::User), 1);
    EXPECT_EQ(toIndex(ProcessColumn::Name), 10);
}

TEST(ProcessColumnConfigTest, ToIndexIsMonotonic)
{
    // Verify indices are sequential (no gaps)
    const auto count = processColumnCount();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto col = static_cast<ProcessColumn>(i);
        EXPECT_EQ(toIndex(col), i);
    }
}

TEST(ProcessColumnConfigTest, AllColumnsContainsUniqueColumns)
{
    constexpr auto columns = allProcessColumns();
    std::vector<ProcessColumn> seen;
    seen.reserve(columns.size());

    for (const auto col : columns)
    {
        // Check not seen before
        EXPECT_EQ(std::find(seen.begin(), seen.end(), col), seen.end()) << "Duplicate column detected";
        seen.push_back(col);
    }
}

// ========== Column Settings ==========

TEST(ProcessColumnSettingsTest, DefaultConstructorHasDefaultVisibility)
{
    const ProcessColumnSettings settings;

    // Most columns should be visible by default
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Pid));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Cpu));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Memory));
}

TEST(ProcessColumnSettingsTest, SetVisibilityChangesState)
{
    ProcessColumnSettings settings;

    // Hide a column
    settings.setVisible(ProcessColumn::Pid, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::Pid));

    // Show it again
    settings.setVisible(ProcessColumn::Pid, true);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Pid));
}

TEST(ProcessColumnSettingsTest, ToggleVisibilityFlipsState)
{
    ProcessColumnSettings settings;

    const bool initial = settings.isVisible(ProcessColumn::Name);
    settings.toggleVisible(ProcessColumn::Name);
    EXPECT_EQ(settings.isVisible(ProcessColumn::Name), !initial);

    // Toggle back
    settings.toggleVisible(ProcessColumn::Name);
    EXPECT_EQ(settings.isVisible(ProcessColumn::Name), initial);
}

TEST(ProcessColumnSettingsTest, BoundaryConditions)
{
    ProcessColumnSettings settings;

    // Test all valid columns can be toggled
    const auto count = processColumnCount();
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto col = static_cast<ProcessColumn>(i);
        const bool before = settings.isVisible(col);
        settings.toggleVisible(col);
        EXPECT_NE(settings.isVisible(col), before);
        settings.toggleVisible(col);
        EXPECT_EQ(settings.isVisible(col), before);
    }
}

TEST(ProcessColumnSettingsTest, AllColumnsCanBeHidden)
{
    ProcessColumnSettings settings;

    // Hide all columns
    const auto columns = allProcessColumns();
    for (const auto col : columns)
    {
        settings.setVisible(col, false);
    }

    // Verify all hidden
    for (const auto col : columns)
    {
        EXPECT_FALSE(settings.isVisible(col));
    }
}

TEST(ProcessColumnSettingsTest, AllColumnsCanBeShown)
{
    ProcessColumnSettings settings;

    // Show all columns
    const auto columns = allProcessColumns();
    for (const auto col : columns)
    {
        settings.setVisible(col, true);
    }

    // Verify all shown
    for (const auto col : columns)
    {
        EXPECT_TRUE(settings.isVisible(col));
    }
}

} // namespace
} // namespace App
