#pragma once

#include "Domain/StorageModel.h"

#include <chrono>

namespace App::StorageSection
{

/// Context struct containing all state needed to render the storage/disk I/O section.
/// This allows the render function to be extracted from NetworkSection
/// without requiring access to private members.
struct RenderContext
{
    // Models (non-owning pointer)
    Domain::StorageModel* storageModel = nullptr;

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
};

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
