#pragma once

#include "Domain/SystemModel.h"
#include "Domain/SystemSnapshot.h"
#include "UI/ChartWidgets.h"

#include <chrono>
#include <cstdint>
#include <vector>

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
    double historyScrollSeconds = 0.0;
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

/// Render the Memory & Swap history chart with now bars
/// @param ctx Render context with model, history config, and smoothed values
/// @param timestamps History timestamps from system model
/// @param nowSeconds Current time in seconds
/// @param nowBarColumns Number of columns for now bars layout
void renderMemorySection(RenderContext& ctx, const std::vector<double>& timestamps, double nowSeconds, int nowBarColumns);

} // namespace App::MemorySection
