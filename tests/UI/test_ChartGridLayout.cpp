/// @file test_ChartGridLayout.cpp
/// @brief Tests for UI::Widgets::computeChartGridLayout(), the pure grid-sizing math shared by
/// CpuCoresSection's per-core grid and StorageSection's per-disk grid (see UI/ChartGrid.h for
/// the ImGui rendering helper built on top of this).

#include "UI/ChartGridLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>

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

TEST(ChartGridLayoutTest, NoFittingShapeFallsBackToSingleColumn)
{
    // Regression coverage for a real bug: when no column count lets width and height both fit
    // (a genuinely too-small panel for the item count), the score-based loop above can still
    // favor a high column count because its score is computed from a *clamped* cellWidth that a
    // ImGuiTableFlags_SizingStretchSame table will never actually grant -- that table always
    // divides availableWidth evenly across the chosen columns, ignorant of minCellWidth. Left
    // unhandled, that mismatch crushes every chart's rendered width far below minCellWidth
    // instead of falling back to the intended last resort: fewer columns and vertical scrolling.
    // A single column is the only shape that's ever safe to choose when nothing truly fits: the
    // renderer (ChartGrid.h) sizes each cell's actual width by stretching it to its table
    // column's share of availableWidth, not by cellWidth below (which stays a clamped floor
    // value here, same as TinyPanelClampsToMinimumCellSize above) -- so columns=1 is what
    // actually gives each chart the widest cell a single column can provide, deferring all
    // overflow to rows, which grid-level scrolling can handle.
    const ChartGridConfig config{
        .availableWidth = 300.0F, .availableHeight = 900.0F, .itemCount = 8, .minCellWidth = 320.0F, .minCellHeight = 140.0F};
    const auto grid = computeChartGridLayout(config);

    EXPECT_EQ(grid.columns, 1U);
    EXPECT_EQ(grid.rows, 8U);
}

TEST(ChartGridLayoutTest, RowAndColumnOverheadAreReservedSoTotalFitsExactly)
{
    // columnOverhead/rowOverhead model a fixed per-cell cost the *renderer* adds outside
    // computeChartGridLayout's own cellWidth/cellHeight (e.g. the wrapping table's CellPadding).
    // A caller that adds that overhead back around each cell should land on exactly the
    // available space, not exceed it -- this is what stopped an outer scrollbar from appearing
    // around the whole grid even when every individual cell fit its own space (#823 review).
    const ChartGridConfig config{
        .availableWidth = 1000.0F, .availableHeight = 1000.0F, .itemCount = 4, .columnOverhead = 8.0F, .rowOverhead = 4.0F};
    const auto grid = computeChartGridLayout(config);

    ASSERT_EQ(grid.columns, 2U);
    ASSERT_EQ(grid.rows, 2U);
    EXPECT_LE(static_cast<float>(grid.columns) * (grid.cellWidth + config.columnOverhead), config.availableWidth + 0.01F);
    EXPECT_LE(static_cast<float>(grid.rows) * (grid.cellHeight + config.rowOverhead), config.availableHeight + 0.01F);
}

TEST(ChartGridLayoutTest, RealisticCoreCountsAndWindowSizesNeverOverflowWhenAFitExists)
{
    // Regression coverage for a real bug found by manual testing: scoring/selecting a candidate
    // by its *raw* division (before minCellWidth/minCellHeight and the table's row/column
    // overhead are applied) could pick a shape that, once those were added back, rendered taller
    // or wider than the available space -- forcing an outer scrollbar around an otherwise
    // reasonably-sized grid (observed with 10 CPU cores on a normal-sized window). Sweep a range
    // of realistic core counts and window sizes and assert the chosen grid never needs to
    // overflow when a non-overflowing shape actually exists for that item count/space.
    constexpr float columnOverhead = 8.0F; // e.g. table CellPadding.x * 2
    constexpr float rowOverhead = 4.0F;    // e.g. table CellPadding.y * 2
    constexpr float minCellWidth = 240.0F;
    constexpr float minCellHeight = 100.0F;

    const std::array<size_t, 9> coreCounts{1, 2, 4, 6, 8, 10, 12, 16, 24};
    const std::array<float, 4> widths{1000.0F, 1280.0F, 1600.0F, 1920.0F};
    const std::array<float, 4> heights{600.0F, 800.0F, 900.0F, 1080.0F};

    for (const size_t itemCount : coreCounts)
    {
        for (const float width : widths)
        {
            for (const float height : heights)
            {
                // Reference oracle: does *any* column count let both width and height fit at the
                // configured floors? Some combinations here are genuinely infeasible (e.g. 24
                // items at minCellWidth=240/minCellHeight=100 need more area than a 1000x600
                // window has regardless of shape) -- those must still fall back to scrolling, so
                // only assert non-overflow where a fit is actually possible.
                bool aFitExists = false;
                for (size_t columns = 1; columns <= itemCount && !aFitExists; ++columns)
                {
                    const auto rows = static_cast<float>((itemCount + columns - 1) / columns);
                    const auto columnsF = static_cast<float>(columns);
                    const float cw = std::max((width - (columnsF * columnOverhead)) / columnsF, minCellWidth);
                    const float ch = std::max((height - (rows * rowOverhead)) / rows, minCellHeight);
                    aFitExists = ((cw + columnOverhead) * columnsF <= width + 0.5F) && ((ch + rowOverhead) * rows <= height + 0.5F);
                }
                if (!aFitExists)
                {
                    continue;
                }

                const ChartGridConfig config{.availableWidth = width,
                                             .availableHeight = height,
                                             .itemCount = itemCount,
                                             .minCellWidth = minCellWidth,
                                             .minCellHeight = minCellHeight,
                                             .columnOverhead = columnOverhead,
                                             .rowOverhead = rowOverhead};
                const auto grid = computeChartGridLayout(config);

                const bool totalWidthFits = (static_cast<float>(grid.columns) * (grid.cellWidth + columnOverhead)) <= width + 0.5F;
                const bool totalHeightFits = (static_cast<float>(grid.rows) * (grid.cellHeight + rowOverhead)) <= height + 0.5F;

                EXPECT_TRUE(totalWidthFits) << "itemCount=" << itemCount << " width=" << width << " height=" << height
                                            << " columns=" << grid.columns << " cellWidth=" << grid.cellWidth;
                EXPECT_TRUE(totalHeightFits) << "itemCount=" << itemCount << " width=" << width << " height=" << height
                                             << " rows=" << grid.rows << " cellHeight=" << grid.cellHeight;
            }
        }
    }
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
