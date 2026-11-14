#pragma once

#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "UI/ChartWidgets.h"
#include "UI/InlineText.h"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>

namespace App::MemorySection
{

/// Smoothed memory values for display
struct SmoothedMemory
{
    double usedPercent = 0.0;
    double cachedPercent = 0.0;
    double swapPercent = 0.0;
    bool initialized = false;
};

/// Context required to render the memory section
struct RenderContext
{
    const Domain::SystemPublication* publication = nullptr;
    // Generation of `publication` and the timestamps passed with it (UI::Widgets::nextChartDataGeneration()),
    // so the chart keeps its reduced points until it changes (HistoryChartConfig::dataGeneration, #1139).
    std::uint64_t chartDataGeneration = 0;
    double maxHistorySeconds = 60.0;
    float lastDeltaSeconds = 0.0F;
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values (owned by caller, modified by render functions)
    SmoothedMemory* smoothedMemory = nullptr;

    // Height of the history plot. The Overview tab sizes its charts to share the height available
    // (UI/HistoryPlotHeight.h) and passes that in.
    float plotHeight = UI::Widgets::HISTORY_PLOT_HEIGHT_DEFAULT;
};

/// Update smoothed memory values based on current snapshot
/// @param smoothed The smoothed values to update
/// @param snap Current system snapshot
/// @param deltaTimeSeconds Time since last update
/// @param refreshInterval Sampling interval for alpha calculation
void updateSmoothedMemory(SmoothedMemory& smoothed,
                          const Domain::SystemSnapshot& snap,
                          float deltaTimeSeconds,
                          std::chrono::milliseconds refreshInterval);

/// The value strip's Commit entry (#1627): "35.0 GiB / 73.7 GiB (48%)", commit charge against the
/// commit limit; empty when the snapshot has no commit charge. Held in place, as the strip shows it
/// every frame (#843).
[[nodiscard]] UI::InlineText commitStripValue(const Domain::SystemSnapshot& snap);

/// The Commit entry's hover text: what it measures, and the peak commit when the platform keeps one
/// (Windows); empty when the snapshot has no commit charge.
[[nodiscard]] std::string commitStripTooltip(const Domain::SystemSnapshot& snap);

/// The value strip's Compressed entry (#3087): "2.9× (1.2 GiB in 410 MiB)", compressed swap's ratio and
/// sizes, zram and zswap together; on Windows (#3363) the compression store's size alone, "410 MiB"; empty
/// while nothing compressed is stored or compression is off. Held in place, like commitStripValue().
[[nodiscard]] UI::InlineText compressedStripValue(const Domain::SystemSnapshot& snap);

/// The Compressed entry's hover text: each kind's sizes and ratio, zram's RAM use, and the ratio's range
/// over @p ratioHistory (the chart's window; NaN where nothing was stored); on Windows (#3363) what the size
/// is, with no ratio; empty with no entry.
[[nodiscard]] std::string compressedStripTooltip(const Domain::SystemSnapshot& snap, std::span<const float> ratioHistory);

/// Render the Memory & Swap history chart with now bars
/// @param ctx Render context with model, history config, and smoothed values
/// @param timestamps History timestamps from system model
/// @param nowSeconds Current time in seconds
/// @param nowBarColumns Number of columns for now bars layout
void renderMemorySection(RenderContext& ctx, std::span<const double> timestamps, double nowSeconds, int nowBarColumns);

} // namespace App::MemorySection
