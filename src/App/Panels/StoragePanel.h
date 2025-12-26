#pragma once

#include "App/Panel.h"
#include "Domain/StorageModel.h"
#include "UI/Theme.h"

#include <chrono>
#include <memory>

namespace App
{

/// Panel displaying storage/disk I/O metrics with ImPlot graphs.
/// Shows per-device read/write rates, utilization, and I/O operations.
class StoragePanel : public Panel
{
  public:
    StoragePanel();
    ~StoragePanel() override;

    StoragePanel(const StoragePanel&) = delete;
    StoragePanel& operator=(const StoragePanel&) = delete;
    StoragePanel(StoragePanel&&) = delete;
    StoragePanel& operator=(StoragePanel&&) = delete;

    /// Initialize the panel (creates StorageModel).
    void onAttach() override;

    /// Cleanup.
    void onDetach() override;

    /// Update logic (refresh cadence is driven by main loop).
    void onUpdate(float deltaTime) override;

    /// Set the refresh interval.
    void setSamplingInterval(std::chrono::milliseconds interval);

    /// Request an immediate refresh.
    void requestRefresh();

    /// Render the panel.
    void render(bool* open) override;

  private:
    void renderOverview();
    void renderDeviceDetails();

    std::unique_ptr<Domain::StorageModel> m_Model;

    double m_MaxHistorySeconds = 300.0;
    std::chrono::milliseconds m_RefreshInterval{1000};
    float m_RefreshAccumulatorSec = 0.0F;
    bool m_ForceRefresh = false;

    // Smoothed values for overview display
    struct SmoothedDisk
    {
        double readMBps = 0.0;
        double writeMBps = 0.0;
        double utilization = 0.0;
        bool initialized = false;
    };
    std::unordered_map<std::string, SmoothedDisk> m_SmoothedDisks;

    // Cached layout values
    UI::FontSize m_LastFontSize = UI::FontSize::Medium;
    float m_OverviewLabelWidth = 0.0F;
    bool m_LayoutDirty = true;

    void updateCachedLayout();
    void updateSmoothedMetrics(const Domain::StorageSnapshot& snap, float deltaTimeSeconds);
};

} // namespace App
