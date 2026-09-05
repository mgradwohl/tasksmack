/// @file test_ChartGridLayout.cpp
/// @brief Tests for UI::Widgets::computeChartGridLayout(), the pure grid-sizing math shared by
/// CpuCoresSection's per-core grid and StorageSection's per-disk grid (see UI/ChartGrid.h for
/// the ImGui rendering helper built on top of this).

#include "UI/ChartGridLayout.h"

#include <gtest/gtest.h>

namespace UI::Widgets
{
namespace
{

TEST(ChartGridLayoutTest, ZeroItemsReturnsSingleFullSizeCell)
{
    const ChartGridConfig config{.availableWidth = 1000.0F, .availableHeight = 500.0F, .itemCount = 0};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 1U);
    EXPECT_EQ(grid.rows, 1U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 1000.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 500.0F);
}

TEST(ChartGridLayoutTest, SingleItemFillsWholePanel)
{
    const ChartGridConfig config{.availableWidth = 1000.0F, .availableHeight = 500.0F, .itemCount = 1};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 1U);
    EXPECT_EQ(grid.rows, 1U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 1000.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 500.0F);
}

TEST(ChartGridLayoutTest, SixteenItemsOnSquarePanelResolveToFourByFour)
{
    // The example from the design discussion: a square panel with 16 items should resolve to a
    // clean, square-ish 4x4 grid, not a width-only column count.
    const ChartGridConfig config{.availableWidth = 1600.0F, .availableHeight = 1600.0F, .itemCount = 16};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 4U);
    EXPECT_EQ(grid.rows, 4U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 400.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 400.0F);
}

TEST(ChartGridLayoutTest, SixteenItemsOnWidePanelPreferFewerRowsMoreColumns)
{
    // A 4:1 wide panel should favor a flatter grid (more columns, fewer rows) than the square
    // case above, so the resulting cells stay close to square instead of being tall slivers.
    const ChartGridConfig config{.availableWidth = 6400.0F, .availableHeight = 1600.0F, .itemCount = 16};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 8U);
    EXPECT_EQ(grid.rows, 2U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 800.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 800.0F);
}

TEST(ChartGridLayoutTest, SixteenItemsOnTallPanelPreferFewerColumnsMoreRows)
{
    // Mirror image of the wide case: a 1:4 tall panel should favor more rows, fewer columns.
    const ChartGridConfig config{.availableWidth = 1600.0F, .availableHeight = 6400.0F, .itemCount = 16};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 2U);
    EXPECT_EQ(grid.rows, 8U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 800.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 800.0F);
}

TEST(ChartGridLayoutTest, ItemCountNotDivisibleByColumnsStillFillsSpace)
{
    // 5 items on a square panel: 2x3 (6 cells, 1 wasted) ties 3x2 on both score and waste: the
    // implementation deterministically prefers the smaller column count on ties.
    const ChartGridConfig config{.availableWidth = 1000.0F, .availableHeight = 1000.0F, .itemCount = 5};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 2U);
    EXPECT_EQ(grid.rows, 3U);
    EXPECT_FLOAT_EQ(grid.cellWidth, 500.0F);
    EXPECT_FLOAT_EQ(grid.cellHeight, 1000.0F / 3.0F);
}

TEST(ChartGridLayoutTest, TinyPanelClampsToMinimumCellSize)
{
    // A panel too small to give every one of 8 items its minimum cell size should still produce
    // a valid grid, with cells clamped to the configured floor rather than shrinking further.
    const ChartGridConfig config{
        .availableWidth = 300.0F, .availableHeight = 200.0F, .itemCount = 8, .minCellWidth = 240.0F, .minCellHeight = 140.0F};
    const auto grid = computeChartGridLayout(config);

    EXPECT_GE(grid.cellWidth, config.minCellWidth);
    EXPECT_GE(grid.cellHeight, config.minCellHeight);
    EXPECT_GE(grid.columns * grid.rows, config.itemCount);
}

TEST(ChartGridLayoutTest, NonPositiveTargetAspectFallsBackToSquare)
{
    // A misconfigured (non-positive) target aspect must not divide-by-zero or misbehave; it
    // should fall back to the same square-cell behavior as the default.
    const ChartGridConfig squareConfig{.availableWidth = 1600.0F, .availableHeight = 1600.0F, .itemCount = 16};
    const ChartGridConfig zeroAspectConfig{
        .availableWidth = 1600.0F, .availableHeight = 1600.0F, .itemCount = 16, .targetCellAspect = 0.0F};

    const auto squareGrid = computeChartGridLayout(squareConfig);
    const auto zeroAspectGrid = computeChartGridLayout(zeroAspectConfig);

    EXPECT_EQ(zeroAspectGrid.columns, squareGrid.columns);
    EXPECT_EQ(zeroAspectGrid.rows, squareGrid.rows);
}

} // namespace
} // namespace UI::Widgets
