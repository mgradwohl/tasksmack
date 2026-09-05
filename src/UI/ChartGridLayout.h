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

    size_t bestColumns = 1;
    float bestScore = -1.0F;
    size_t bestWaste = config.itemCount; // rows*cols - itemCount for the current best

    for (size_t columns = 1; columns <= config.itemCount; ++columns)
    {
        const size_t rows = (config.itemCount + columns - 1) / columns;
        const auto columnsF = static_cast<float>(columns);
        const auto rowsF = static_cast<float>(rows);

        const float cellWidth = safeWidth / columnsF;
        const float cellHeight = safeHeight / rowsF;
        const float score = std::min(cellWidth, cellHeight * aspect);
        const size_t waste = (rows * columns) - config.itemCount;

        const bool better = score > bestScore || (score == bestScore && waste < bestWaste);
        if (better)
        {
            bestScore = score;
            bestColumns = columns;
            bestWaste = waste;
        }
    }

    const size_t bestRows = (config.itemCount + bestColumns - 1) / bestColumns;
    const float finalCellWidth = std::max(safeWidth / static_cast<float>(bestColumns), config.minCellWidth);
    const float finalCellHeight = std::max(safeHeight / static_cast<float>(bestRows), config.minCellHeight);

    return {.columns = bestColumns, .rows = bestRows, .cellWidth = finalCellWidth, .cellHeight = finalCellHeight};
}

} // namespace UI::Widgets
