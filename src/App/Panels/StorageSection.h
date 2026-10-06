#pragma once

#include "Domain/StorageModel.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace App::StorageSection
{

/// Smoothed NowBar values for one disk in the per-disk grid, so its bars ease like every other
/// NowBar instead of stepping to each new sample (#1012).
struct SmoothedDiskRates
{
    double readBytesPerSec = 0.0;
    double writeBytesPerSec = 0.0;
    bool initialized = false;
};

/// Context struct containing all state needed to render the storage/disk I/O section.
/// This allows the render function to be extracted from NetworkSection
/// without requiring access to private members.
struct RenderContext
{
    // Models (non-owning pointer)
    const Domain::StoragePublication* publication = nullptr;
    // Generation of the histories charted (UI::Widgets::nextChartDataGeneration()), so the charts keep
    // their reduced points until it changes (HistoryChartConfig::dataGeneration, #1139). 0: none.
    std::uint64_t chartDataGeneration = 0;

    // History configuration
    double maxHistorySeconds = 300.0;
    double historyScrollSeconds = 0.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values for disk I/O (passed by reference so we can update them)
    double* smoothedReadBytesPerSec = nullptr;
    double* smoothedWriteBytesPerSec = nullptr;
    bool* smoothedInitialized = nullptr;

    // Per-disk smoothed NowBar values, keyed by device name. Null: the per-disk bars show raw values.
    std::unordered_map<std::string, SmoothedDiskRates>* smoothedPerDisk = nullptr;

    // The tab's shared chart height (#959). Used by the single-disk chart; the per-disk grid takes
    // whatever height is left instead. Null keeps the fixed default height.
    UI::Widgets::FillPlotLayout* fill = nullptr;
};

/// Whether the section shows one chart per disk in a grid that fills the remaining height, rather
/// than a single aggregate chart.
[[nodiscard]] inline bool usesDiskGrid(const Domain::StoragePublication* publication) noexcept
{
    return (publication != nullptr) && (publication->perDiskHistory.size() > 1);
}

/// Least height the per-disk grid needs, its heading included, when the section is @p availableWidth
/// wide: every row at its minimum cell height (UI::Widgets::computeChartGridMinimumHeight()). 0 when
/// the section draws no grid. The Network and I/O tab reserves it before sizing the chart above
/// the grid, so the chart cannot take the grid's rows and push them into scrolling (#1370 review).
[[nodiscard]] float diskGridMinimumHeight(const Domain::StoragePublication* publication, float availableWidth);

/// Render the Disk I/O section with history chart and now bars.
/// @param ctx Render context containing model and smoothed values
void renderStorageSection(RenderContext& ctx);

/// Update smoothed disk I/O values for external callers (e.g., Overview tab).
/// @param targetRead Current read bytes per second
/// @param targetWrite Current write bytes per second
/// @param deltaTimeSeconds Time since last update
/// @param ctx Render context containing smoothed value pointers
void updateSmoothedDiskIO(double targetRead, double targetWrite, float deltaTimeSeconds, RenderContext& ctx);

} // namespace App::StorageSection
