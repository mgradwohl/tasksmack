#pragma once

#include "Domain/SystemModel.h"

#include <chrono>
#include <vector>

namespace App::CpuCoresSection
{

/// Context struct containing all state needed to render the CPU Cores section.
/// This allows the render function to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Model (non-owning pointer)
    Domain::SystemModel* systemModel = nullptr;

    // Cached timestamps from model (for efficiency)
    const std::vector<double>* timestampsCache = nullptr;

    // History configuration
    double maxHistorySeconds = 300.0;
    double historyScrollSeconds = 0.0;
    float lastDeltaSeconds = 0.0F;

    // Refresh interval for smoothing alpha calculation
    std::chrono::milliseconds refreshInterval{1000};

    // Smoothed per-core CPU percentages (vector indexed by core ID)
    std::vector<double>* smoothedPerCore = nullptr;
};

/// Render the CPU Cores section with per-core utilization charts.
/// @param ctx Render context containing model and smoothed values
void renderCpuCoresSection(RenderContext& ctx);

/// Update smoothed values for all CPU cores.
/// @param snap Current system snapshot
/// @param ctx Render context (uses refreshInterval and lastDeltaSeconds)
void updateSmoothedPerCore(const Domain::SystemSnapshot& snap, RenderContext& ctx);

} // namespace App::CpuCoresSection
