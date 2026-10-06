#pragma once

#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "FpsCounter.h"
#include "PanelTabs.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "TabLabel.h"

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <string>
#include <vector>

namespace App
{

class TitleBarLayer;

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
    void onSDLEvent(SDL_Event* event) override;

    /// The custom title bar, when there is one (not with native decorations, #745). It owns the
    /// window's minimum size then, and is handed the panels' share of it each frame (#1207).
    /// Non-owning; the application's layer stack outlives both layers' use of it.
    void setTitleBar(TitleBarLayer* titleBar) noexcept
    {
        m_TitleBar = titleBar;
    }

  private:
    void renderTabBar();
    void renderStatusBar() const;
    void applyBaseMinimumWindowSize();
    void applyContentMinimumSize(float widthPx, float heightPx);

    // Panels
    ProcessesPanel m_ProcessesPanel;
    ProcessDetailsPanel m_ProcessDetailsPanel;
    SystemMetricsPanel m_SystemMetricsPanel;

    // Frame timing / FPS display
    FpsCounter m_FpsCounter;

    // GPU debug logging throttling
    std::int32_t m_LastGpuLogPid = -1;
    bool m_LastGpuLogHasData = false;

    // The PID the process model is watching for Process Details (ProcessModel::watchProcess()), and
    // the scratch buffer its new samples are collected into each frame, reused to avoid allocating.
    std::int32_t m_WatchedPid = -1;
    std::vector<Domain::ProcessSample> m_PendingSamples;

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
    // Display the minimum was last capped to the usable bounds of; re-applied on a move to another
    // display (#1207).
    SDL_DisplayID m_MinimumSizeDisplayId = 0;
    // Size the panels need (#1207, #1278): handed to the title bar, or with native decorations part
    // of the minimum applied here. Whole pixels, so it is re-applied only when it really changes.
    int m_ContentMinimumWidthPx = 0;
    int m_ContentMinimumHeightPx = 0;
    TitleBarLayer* m_TitleBar = nullptr;

    // Render Metrics overlay (per-chart vertex count and CPU cost). Toggled with Ctrl+Shift+M.
    bool m_ShowRenderMetrics = false;

    // Cached tab labels — rebuilt only when the underlying data changes, not every frame.
    // Avoids per-frame heap allocations from string concatenation in renderTabBar().
    // Both carry a fixed "###" ID suffix (TabLabel.h), so a new name never changes which tab ImGui
    // thinks is selected (#1140).
    std::string m_CachedSystemTabLabel;      // ICON + hostname: rebuilt in onAttach()
    TabLabel::CachedLabel m_DetailsTabLabel; // ICON + process name: rebuilt when the name changes

    // Declared last so panels and cached labels outlive the non-owning registry.
    PanelTabs m_Tabs;
};

} // namespace App
