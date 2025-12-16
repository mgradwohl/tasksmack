#pragma once

#include "App/Panel.h"
#include "Domain/SystemModel.h"
#include "UI/Theme.h"

#include <implot.h>

#include <memory>
#include <thread>

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

    /// Initialize the panel (creates SystemModel and starts sampler thread).
    void onAttach() override;

    /// Cleanup (stops sampler thread).
    void onDetach() override;

    /// Render the panel.
    void render(bool* open) override;

  private:
    void renderOverview();
    void renderCpuSection();
    void renderMemorySection();
    void renderPerCoreSection();
    void samplerLoop();

    std::unique_ptr<Domain::SystemModel> m_Model;

    // Background sampler
    std::jthread m_SamplerThread;
    std::atomic<bool> m_Running{false};

    // Display options
    bool m_ShowPerCore = false;

    // Heatmap colormap (cached to avoid recreating every frame)
    ImPlotColormap m_HeatmapColormap = -1;
    UI::ThemeId m_LastThemeId = UI::ThemeId::ArcticFire;

    // Cached layout values (recalculated one frame after font changes)
    UI::FontSize m_LastFontSize = UI::FontSize::Medium;
    float m_OverviewLabelWidth = 0.0F;
    float m_PerCoreLabelWidth = 0.0F;
    size_t m_LastCoreCount = 0;
    bool m_LayoutDirty = true; // Start dirty to calculate on first frame

    void updateCachedLayout();
};

} // namespace App
