#pragma once

// Pure grid-sizing math for "N identical small charts" panels (per-core CPU, per-disk I/O,
// and any future per-item chart grid). Extracted so the layout decision is directly
// unit-testable without a live ImGui context -- see UI/ChartGrid.h for the ImGui-calling
// rendering helper built on top of this.

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace UI::Widgets
{

/// Inputs for computeChartGridLayout(). availableWidth/availableHeight are the panel's
/// remaining content region (e.g. ImGui::GetContentRegionAvail()) at the point the grid
/// starts rendering.
struct ChartGridConfig
{
    float availableWidth = 0.0F;
    float availableHeight = 0.0F;
    size_t itemCount = 0;
    float minCellWidth = 240.0F;
    float minCellHeight = 140.0F;
    // Width/height a single chart cell "wants" when given free rein. 1.0 (square cells) makes
    // the overall grid SHAPE track the panel's shape -- a square panel resolves to a square-ish
    // grid (e.g. 4x4 for 16 items), a wide panel to fewer rows/more columns (e.g. 2x8) -- which
    // is what makes "fill the panel intelligently" visible to the user. Raise it (e.g. 1.6) for
    // a caller that wants individual cells biased landscape instead, at the cost of the grid
    // itself tracking the panel's aspect ratio less closely.
    float targetCellAspect = 1.0F;
    // Fixed width/height the rendering table itself adds around every cell, independent of the
    // cell's own content (e.g. the table's CellPadding on each side) -- set by the caller so the
    // chosen cellWidth/cellHeight, once that per-cell overhead is added back by the renderer,
    // sums to exactly availableWidth/availableHeight instead of exceeding it and forcing an
    // outer scrollbar. Zero by default for pure-math callers/tests that don't render into a table.
    float columnOverhead = 0.0F;
    float rowOverhead = 0.0F;
};

/// Chosen grid shape and resulting per-cell size.
struct ChartGridDimensions
{
    size_t columns = 1;
    size_t rows = 1;
    float cellWidth = 0.0F;
    float cellHeight = 0.0F;
};

/// Choose a rows x columns grid for config.itemCount cells that fills the available
/// width/height while keeping individual cells close to targetCellAspect, instead of a
/// fixed cell size that leaves the panel's extra space unused (or overflows it).
///
/// For each candidate column count, the resulting cell is scored by the size of the
/// largest targetCellAspect-shaped box that fits inside it -- the same idea video-call
/// gallery-view grids use to pick a tile layout. Ties (equal score) prefer the candidate
/// with fewer empty trailing cells, so e.g. 16 items on a square panel resolves to a
/// clean 4x4 rather than an equally-scored but wasteful 5x4 (20 cells, 4 empty).
///
/// itemCount is expected to be small (tens, not thousands -- core counts, disk counts),
/// so the O(itemCount) candidate search is negligible even run every frame; no caching
/// is needed on top of it.
[[nodiscard]] inline auto computeChartGridLayout(const ChartGridConfig& config) -> ChartGridDimensions
{
    if (config.itemCount == 0)
    {
        return {.columns = 1,
                .rows = 1,
                .cellWidth = std::max(config.availableWidth, 0.0F),
                .cellHeight = std::max(config.availableHeight, 0.0F)};
    }

    const float safeWidth = std::max(config.availableWidth, config.minCellWidth);
    const float safeHeight = std::max(config.availableHeight, config.minCellHeight);
    const float aspect = config.targetCellAspect > 0.0F ? config.targetCellAspect : 1.0F;
    // Relative slack for the "does this candidate fit" check below, absorbing float rounding in
    // e.g. columnsF * (safeWidth / columnsF) reproducing safeWidth imprecisely.
    constexpr float FIT_TOLERANCE = 1e-3F;

    size_t bestColumns = 1;
    float bestScore = -1.0F;
    size_t bestWaste = config.itemCount; // rows*cols - itemCount for the current best
    bool bestFits = false;

    for (size_t columns = 1; columns <= config.itemCount; ++columns)
    {
        const size_t rows = (config.itemCount + columns - 1) / columns;
        const auto columnsF = static_cast<float>(columns);
        const auto rowsF = static_cast<float>(rows);

        // Divide up the space left *after* reserving each row/column's fixed table overhead,
        // so columns*(cellWidth+columnOverhead) and rows*(cellHeight+rowOverhead) reproduce
        // safeWidth/safeHeight instead of exceeding them once the renderer adds that overhead
        // back around each cell.
        const float widthForCells = std::max(0.0F, safeWidth - (columnsF * config.columnOverhead));
        const float heightForCells = std::max(0.0F, safeHeight - (rowsF * config.rowOverhead));

        // Score (and the fit check just below) use the *clamped* cell size, not the raw
        // division: scoring on the raw size let a candidate whose minCellHeight/minCellWidth
        // floor pushed its actual rendered size past what naturally divides into the available
        // space still win, silently overflowing the panel and forcing an unwanted outer
        // scrollbar even though a differently-shaped grid would have fit cleanly.
        const float cellWidth = std::max(widthForCells / columnsF, config.minCellWidth);
        const float cellHeight = std::max(heightForCells / rowsF, config.minCellHeight);
        const float score = std::min(cellWidth, cellHeight * aspect);
        const size_t waste = (rows * columns) - config.itemCount;
        const bool fits = ((cellWidth + config.columnOverhead) * columnsF <= safeWidth * (1.0F + FIT_TOLERANCE)) &&
                          ((cellHeight + config.rowOverhead) * rowsF <= safeHeight * (1.0F + FIT_TOLERANCE));

        // Two candidates commonly tie exactly (e.g. an NxM and MxN split of a square panel
        // compute the same ratio via swapped operands), so tie-detection needs a tolerance
        // rather than `==` on floats. Scores are pixel dimensions, which can range from tens
        // (a cramped panel) to thousands (a large/4K panel), so the tolerance is scaled by the
        // score's own magnitude rather than a fixed absolute value -- a fixed epsilon that's
        // meaningful at 300px would be far too tight to catch rounding-level near-ties at
        // 3000px, and far too loose relative to a 30px score.
        constexpr float SCORE_TIE_RELATIVE_EPSILON = 1e-4F;
        const float tieThreshold = SCORE_TIE_RELATIVE_EPSILON * std::max({1.0F, std::abs(score), std::abs(bestScore)});
        const float scoreDiff = score - bestScore;
        const bool tied = std::abs(scoreDiff) <= tieThreshold;

        // A candidate that fits without overflowing always beats one that doesn't, regardless
        // of score -- scrolling should only ever be a last resort when *no* shape fits (see
        // the fallback below), not a side effect of picking the highest-scoring shape without
        // checking whether its floor-clamped size actually overflows.
        const bool better = (fits != bestFits) ? fits : (scoreDiff > tieThreshold || (tied && waste < bestWaste));
        if (better)
        {
            bestScore = score;
            bestColumns = columns;
            bestWaste = waste;
            bestFits = fits;
        }
    }

    const size_t bestRows = (config.itemCount + bestColumns - 1) / bestColumns;
    const auto bestColumnsF = static_cast<float>(bestColumns);
    const auto bestRowsF = static_cast<float>(bestRows);
    const float finalWidthForCells = std::max(0.0F, safeWidth - (bestColumnsF * config.columnOverhead));
    const float finalHeightForCells = std::max(0.0F, safeHeight - (bestRowsF * config.rowOverhead));
    const float finalCellWidth = std::max(finalWidthForCells / bestColumnsF, config.minCellWidth);
    const float finalCellHeight = std::max(finalHeightForCells / bestRowsF, config.minCellHeight);

    return {.columns = bestColumns, .rows = bestRows, .cellWidth = finalCellWidth, .cellHeight = finalCellHeight};
}

} // namespace UI::Widgets
