#pragma once

#include "App/Panels/NetInterfaceUtils.h"
#include "App/Panels/StorageSection.h"
#include "Domain/StorageModel.h"
#include "Domain/SystemModel.h"
#include "UI/FillPlotLayout.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace App::NetworkSection
{

/// What the Network tab keeps from frame to frame, so that drawing it allocates nothing once warmed up
/// (#1171): the strings and lists built from a publication, rebuilt only when it -- or whatever else
/// each depends on -- changes, rather than every frame. Owned by the panel. UI thread only.
struct FrameCache
{
    /// The publication everything below was built from.
    const Domain::SystemPublication* publication = nullptr;
    std::uint64_t version = 0;

    /// The interface selector's entries (NetInterfaceUtils::interfaceSelectorLabels()), and the
    /// widest of them at dropdownFontSize.
    std::vector<std::string> interfaceNames;
    float dropdownFontSize = -1.0F;
    float dropdownTextWidth = 0.0F;

    /// The selected interface's series labels, "<name> Sent" and "<name> Received", and the chart
    /// title naming it when its history is unavailable, built for labelsName: keyed on the display name
    /// the labels are made from, not the selection index, so a selection that changes this frame or an
    /// index that now names another interface can't keep stale labels.
    bool labelsBuilt = false;
    std::string labelsName;
    std::string interfaceSentLabel;
    std::string interfaceRecvLabel;
    std::string unavailableTitle;

    /// The Interface Status table's rows (NetInterfaceUtils::getInterfaceStatusRows()) and hidden
    /// count, for rowsShowAll and rowsSeenTraffic interfaces seen moving traffic (a set that only grows).
    bool rowsValid = false;
    bool rowsShowAll = false;
    std::size_t rowsSeenTraffic = 0;
    std::vector<Domain::SystemSnapshot::InterfaceSnapshot> statusRows;
    std::size_t hiddenCount = 0;
};

/// Context struct containing all state needed to render network/disk sections.
/// This allows the render functions to be extracted from SystemMetricsPanel
/// without requiring access to private members.
struct RenderContext
{
    // Models (non-owning pointers)
    const Domain::SystemPublication* systemPublication = nullptr;
    const Domain::StoragePublication* storagePublication = nullptr;
    // Generation of the histories charted (UI::Widgets::nextChartDataGeneration()), so the charts keep
    // their reduced points until it changes (HistoryChartConfig::dataGeneration, #1139). 0: none.
    std::uint64_t chartDataGeneration = 0;
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

    // Interface Status table: "Show all" toggle (per session, not saved) and the interfaces seen moving
    // traffic this session, which stay listed while down (#1211). Null: defaults, nothing remembered.
    bool* showAllInterfaces = nullptr;
    NetInterfaceUtils::InterfaceNameSet* interfacesWithTraffic = nullptr;

    // The tab's chart-height measurements from the previous frame (#959). Null keeps the fixed
    // default height for every chart.
    UI::Widgets::PlotFillState* fillState = nullptr;

    // Set by renderNetworkSection() while the tab's fill scope is open; not for callers.
    UI::Widgets::FillPlotLayout* fill = nullptr;

    // Kept across frames by the caller so drawing allocates nothing (#1171). Null: a fresh one for the
    // frame, which draws the same but rebuilds everything.
    FrameCache* cache = nullptr;
};

/// Render the Disk I/O section with history chart.
/// @param ctx Render context containing models and smoothed values
void renderDiskIOSection(RenderContext& ctx);

/// Render the Network section with interface selector and throughput charts.
/// @param ctx Render context containing models and smoothed values
void renderNetworkSection(RenderContext& ctx);

} // namespace App::NetworkSection
