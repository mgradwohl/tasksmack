#pragma once

#include "App/Panels/StorageSection.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <string>
#include <unordered_map>

namespace App::NetworkSection
{

/// Context struct containing all state needed to render network/disk sections.
/// This allows the render functions to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Models (non-owning pointers)
    const Domain::SystemPublication* systemPublication = nullptr;
    const Domain::StoragePublication* storagePublication = nullptr;
    bool hasNetworkCounters = false;

    // History configuration
    double maxHistorySeconds = 300.0;
    double historyScrollSeconds = 0.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed values for disk I/O (passed by reference so we can update them)
    double* smoothedDiskReadBytesPerSec = nullptr;
    double* smoothedDiskWriteBytesPerSec = nullptr;
    bool* smoothedDiskInitialized = nullptr;
    std::unordered_map<std::string, StorageSection::SmoothedDiskRates>* smoothedPerDisk = nullptr;

    // Smoothed values for network (passed by reference so we can update them)
    double* smoothedNetSentBytesPerSec = nullptr;
    double* smoothedNetRecvBytesPerSec = nullptr;
    bool* smoothedNetInitialized = nullptr;

    // Name of the selected network interface (empty = "Total" / all interfaces combined)
    std::string* selectedNetworkInterface = nullptr;

    // The tab's chart-height measurements from the previous frame (#959). Null keeps the fixed
    // default height for every chart.
    UI::Widgets::PlotFillState* fillState = nullptr;

    // Set by renderNetworkSection() while the tab's fill scope is open; not for callers.
    UI::Widgets::FillPlotLayout* fill = nullptr;
};

/// Render the Disk I/O section with history chart.
/// @param ctx Render context containing models and smoothed values
void renderDiskIOSection(RenderContext& ctx);

/// Render the Network section with interface selector and throughput charts.
/// @param ctx Render context containing models and smoothed values
void renderNetworkSection(RenderContext& ctx);

} // namespace App::NetworkSection
