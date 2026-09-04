#include "App/ProcessColumnConfig.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

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
    // Verify the positions of stable anchor columns.
    // These indices are part of the public table-order contract: identity columns come
    // first, Windows-only feature columns (Publisher, Type, GdiObjects) follow in
    // documented order, and no accidental reorder should go undetected.
    EXPECT_EQ(toIndex(ProcessColumn::PID), 0);
    EXPECT_EQ(toIndex(ProcessColumn::Name), 1);
    EXPECT_EQ(toIndex(ProcessColumn::Publisher), 4);   // Windows publisher — after User(2) and PPID(3)
    EXPECT_EQ(toIndex(ProcessColumn::Type), 7);        // Process type — after State(5) and Status(6)
    EXPECT_EQ(toIndex(ProcessColumn::GdiObjects), 18); // GDI count — after Handles(17)
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
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::Name));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::CpuPercent));
    EXPECT_TRUE(settings.isVisible(ProcessColumn::MemPercent));
}

TEST(ProcessColumnSettingsTest, SetVisibilityChangesState)
{
    ProcessColumnSettings settings;

    // Hide a column
    settings.setVisible(ProcessColumn::PID, false);
    EXPECT_FALSE(settings.isVisible(ProcessColumn::PID));

    // Show it again
    settings.setVisible(ProcessColumn::PID, true);
    EXPECT_TRUE(settings.isVisible(ProcessColumn::PID));
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

TEST(ProcessColumnSettingsTest, ToggleVisibilityOnlyAffectsTargetColumn)
{
    ProcessColumnSettings settings;
    const bool memBefore = settings.isVisible(ProcessColumn::MemPercent);

    settings.toggleVisible(ProcessColumn::CpuPercent);

    // Toggling one column must not disturb any other column's visibility.
    EXPECT_EQ(settings.isVisible(ProcessColumn::MemPercent), memBefore);
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
