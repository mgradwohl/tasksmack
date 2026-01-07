#pragma once

#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"

#include <chrono>

namespace App::NetworkSection
{

/// Context struct containing all state needed to render network/disk sections.
/// This allows the render functions to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Models (non-owning pointers)
    Domain::SystemModel* systemModel = nullptr;
    Domain::StorageModel* storageModel = nullptr;

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

    // Smoothed values for network (passed by reference so we can update them)
    double* smoothedNetSentBytesPerSec = nullptr;
    double* smoothedNetRecvBytesPerSec = nullptr;
    bool* smoothedNetInitialized = nullptr;

    // Selected network interface (-1 = "Total" / all interfaces combined)
    int* selectedNetworkInterface = nullptr;
};

/// Render the Disk I/O section with history chart.
/// @param ctx Render context containing models and smoothed values
void renderDiskIOSection(RenderContext& ctx);

/// Render the Network section with interface selector and throughput charts.
/// @param ctx Render context containing models and smoothed values
void renderNetworkSection(RenderContext& ctx);

} // namespace App::NetworkSection

