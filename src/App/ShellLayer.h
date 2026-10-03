#pragma once

#include "Core/Layer.h"
#include "FpsCounter.h"
#include "PanelTabs.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"

#include <cstdint>
#include <string>

namespace App
{

class ShellLayer : public Core::Layer
{
  public:
    ShellLayer();
    ~ShellLayer() override = default;

    ShellLayer(const ShellLayer&) = delete;
    ShellLayer& operator=(const ShellLayer&) = delete;
    ShellLayer(ShellLayer&&) = delete;
    ShellLayer& operator=(ShellLayer&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    void onRender() override;
    void onEvent(Core::Event& event) override;

  private:
    void renderTabBar();
    void renderStatusBar() const;
    void applyBaseMinimumWindowSize();

    // Panels
    ProcessesPanel m_ProcessesPanel;
    ProcessDetailsPanel m_ProcessDetailsPanel;
    SystemMetricsPanel m_SystemMetricsPanel;

    // Frame timing / FPS display
    FpsCounter m_FpsCounter;

    // GPU debug logging throttling
    std::int32_t m_LastGpuLogPid = -1;
    bool m_LastGpuLogHasData = false;

    // Cached privilege status: populated in onAttach() from ProcessModel capabilities.
    // Used by renderStatusBar() to show a persistent lock indicator.
    bool m_HasReducedPrivileges = false;

    // Deferred startup settings: the first onUpdate() raises RefreshRateChangedEvent and
    // HistoryDurationChangedEvent with the loaded config, so panels get their starting values the
    // same way they get later changes (#1079).
    bool m_PendingStartupSettings = true;

    // Deferred startup notice: set in onAttach() if the privilege notice should fire.
    // Dispatched in the first onUpdate() call, after all layers are fully stacked.
    bool m_PendingPrivilegeNotice = false;

    // Display scale the base minimum window size was last set for. With native decorations nothing
    // else sets the minimum, so it is re-applied when the scale changes (#943).
    float m_MinimumSizeDisplayScale = 0.0F;

    // Render Metrics overlay (per-chart vertex count and CPU cost). Toggled with Ctrl+Shift+M.
    bool m_ShowRenderMetrics = false;

    // Cached tab labels — rebuilt only when the underlying data changes, not every frame.
    // Avoids per-frame heap allocations from string concatenation in renderTabBar().
    std::string m_CachedSystemTabLabel;  // ICON + hostname: rebuilt in onAttach()
    std::string m_CachedDetailsTabLabel; // ICON + process name: rebuilt when the name changes
    std::string m_CachedLabelText;       // The panel's label text m_CachedDetailsTabLabel was built from

    // Declared last so panels and cached labels outlive the non-owning registry.
    PanelTabs m_Tabs;
};

} // namespace App
