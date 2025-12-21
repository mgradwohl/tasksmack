#pragma once

#include "App/Panel.h"
#include "Domain/SystemModel.h"
#include "UI/Theme.h"

#include <implot.h>

#include <chrono>
#include <memory>

namespace App
{

/// Panel displaying system-wide metrics with ImPlot graphs.
/// Shows CPU, memory, swap usage over time.
class SystemMetricsPanel : public Panel
{
  public:
    SystemMetricsPanel();
    ~SystemMetricsPanel() override;

    SystemMetricsPanel(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel& operator=(const SystemMetricsPanel&) = delete;
    SystemMetricsPanel(SystemMetricsPanel&&) = delete;
    SystemMetricsPanel& operator=(SystemMetricsPanel&&) = delete;

    /// Initialize the panel (creates SystemModel).
    void onAttach() override;

    /// Cleanup.
    void onDetach() override;

    /// Update logic (refresh cadence is driven by main loop).
    void onUpdate(float deltaTime) override;

    /// Set the refresh interval (applied by onUpdate cadence checks).
    void setSamplingInterval(std::chrono::milliseconds interval);

    /// Request an immediate refresh.
    void requestRefresh();

    /// Render the panel.
    void render(bool* open) override;

  private:
    void renderOverview();
    void renderCpuSection();
    void renderPerCoreSection();
    std::unique_ptr<Domain::SystemModel> m_Model;

    double m_MaxHistorySeconds = 300.0;
    double m_HistoryScrollSeconds = 0.0;
    double m_CurrentNowSeconds = 0.0;
    std::vector<double> m_TimestampsCache;

    std::chrono::milliseconds m_RefreshInterval{1000};
    float m_RefreshAccumulatorSec = 0.0F;
    bool m_ForceRefresh = false;
    float m_LastDeltaSeconds = 0.0F;

    struct SmoothedCpu
    {
        double total = 0.0;
        double user = 0.0;
        double system = 0.0;
        double iowait = 0.0;
        double idle = 0.0;
        bool initialized = false;
    } m_SmoothedCpu;

    struct SmoothedMemory
    {
        double usedPercent = 0.0;
        double swapPercent = 0.0;
        bool initialized = false;
    } m_SmoothedMemory;

    std::vector<double> m_SmoothedPerCore;

    // Cached layout values (recalculated one frame after font changes)
    UI::FontSize m_LastFontSize = UI::FontSize::Medium;
    float m_OverviewLabelWidth = 0.0F;
    float m_PerCoreLabelWidth = 0.0F;
    int m_LastCoreCount = 0;
    bool m_LayoutDirty = true; // Start dirty to calculate on first frame

    // Cached hostname for window title
    std::string m_Hostname = "System";

    void updateCachedLayout();
    void updateSmoothedCpu(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedMemory(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
    void updateSmoothedPerCore(const Domain::SystemSnapshot& snap, float deltaTimeSeconds);
};

} // namespace App
