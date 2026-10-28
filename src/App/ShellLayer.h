#pragma once

#include "Core/Layer.h"
#include "FpsCounter.h"
#include "KeyboardShortcuts.h"
#include "MainTabBarView.h"
#include "PanelTabs.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/ServicesPanel.h"
#include "Panels/StartupPanel.h"
#include "Panels/SystemInfoPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/IPrivilegedBroker.h"
#include "Platform/ProcessTypes.h"
#include "SelectOverride.h"
#include "ShellHost.h"
#include "ShellPanels.h"
#include "TabLabel.h"
#include "TitleBarGeometry.h"

#include <SDL3/SDL_video.h>
#include <imgui.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace App
{

class TitleBarLayer;

class ShellLayer : public Core::Layer
{
  public:
    /// With the real host: Core::Application and its window (ShellHost.cpp).
    ShellLayer();
    /// With the given host; a headless test's fake (#1547).
    explicit ShellLayer(std::unique_ptr<IShellHost> host);
    ~ShellLayer() override = default;

    ShellLayer(const ShellLayer&) = delete;
    ShellLayer& operator=(const ShellLayer&) = delete;
    ShellLayer(ShellLayer&&) = delete;
    ShellLayer& operator=(ShellLayer&&) = delete;

    void onAttach() override;
    void onDetach() override;
    void onUpdate(float deltaTime) override;
    /// While the hidden window draws no frames (#3156): ShellPanels::updateWhileHidden().
    void onHiddenUpdate() override;
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

    /// Whether the Processes tab shows its tree, for tests driving F5 through the whole shell (#1891).
    [[nodiscard]] bool processesTreeViewForTesting() const noexcept
    {
        return m_ProcessesPanel.treeViewEnabled();
    }

    /// The Processes tab's panel, for the UI scenarios that check its state after driving it (#2711).
    [[nodiscard]] const ProcessesPanel& processesPanelForTesting() const noexcept
    {
        return m_ProcessesPanel;
    }

    /// The Process Details tab's panel, for tests that check what it took in while hidden (#3156).
    [[nodiscard]] const ProcessDetailsPanel& processDetailsPanelForTesting() const noexcept
    {
        return m_ProcessDetailsPanel;
    }

    /// Whether a startup selection is still waiting to take effect: TASKSMACK_TAB's tab not yet shown,
    /// or TASKSMACK_SELECT_PID/_NAME's process not yet seen in a snapshot (#1559). ScreenshotLayer
    /// captures only once it is false (#2827).
    [[nodiscard]] bool startupSelectionPending() const noexcept
    {
        return m_StartupTab.pending() || m_ProcessesPanel.startupSelectionPending();
    }

    // TASKSMACK_DEMO's requests (#2827, DemoLayer), through the paths the startup hooks and the UI take.
    // None of them is counted by startupSelectionPending(): the screenshot hook's settle is for startup.

    /// Selects the main tab @p name names, as TASKSMACK_TAB resolves one (a registered id, a documented
    /// alias or a visible label, in that order, SelectOverride::findTab()), asked for until ImGui shows
    /// it selected. False, and nothing requested, when no tab matches.
    [[nodiscard]] bool requestMainTab(std::string_view name);

    /// Selects @p target's process in the Processes table when it next appears in a snapshot, as a
    /// click does; the main tab stays (ProcessesPanel::requestSelection()).
    void requestSelection(const SelectOverride::Target& target);

    /// Brings @p tab of Process Details forward the next time its tabs are drawn, as TASKSMACK_DETAILS_TAB
    /// does; the Process Details main tab itself is selected by a ShowProcessDetailsEvent.
    void requestDetailsTab(SelectOverride::DetailsTab tab);

    /// Brings @p tab of the machine (hostname) tab forward, asked for until it shows, as
    /// TASKSMACK_MACHINE_TAB does (#2869); the machine tab itself is requestMainTab("machine")'s.
    void requestMachineTab(SelectOverride::MachineTab tab);

    /// Draws @p section of Process Details' Overview open the next time the Overview is drawn.
    void requestDetailsSection(SelectOverride::DetailsSection section);

    /// Shows (@p show) or hides the Render Metrics overlay, as Ctrl+Shift+M does (#2923): recording starts
    /// with this frame, and the status bar's FPS readout follows the overlay.
    void requestRenderMetrics(bool show) noexcept;

    /// Whether the Render Metrics overlay is shown: by the chord, TASKSMACK_RENDER_METRICS or a metrics= step.
    [[nodiscard]] bool renderMetricsShown() const noexcept
    {
        return m_ShowRenderMetrics;
    }

  private:
    /// Every registered tab's id and visible text, for TASKSMACK_TAB and the demo's tab= to resolve against.
    [[nodiscard]] std::vector<SelectOverride::TabInfo> tabInfos() const;
    /// The htop-style function keys (#170) that do not depend on the tab: F1 Help (with the shortcut
    /// list, #172), F2 Settings, F10 Quit (the normal close request). At the start of onRender().
    void handleGlobalShortcut(KeyboardShortcuts::ShortcutAction action);
    /// F5 (tree view) and F9 (Kill, confirm dialog only), to the tab drawn this frame: called after
    /// renderTabBar() and before the tabs draw, whose render takes the request this frame.
    void handleTabShortcut(KeyboardShortcuts::ShortcutAction action);
    void renderTabBar();
    /// The Process Details tab's program icon for this frame, and the label that makes room for it (#1800).
    void updateDetailsTabIcon();
    void renderStatusBar() const;
    void applyBaseMinimumWindowSize();
    void applyContentMinimumSize(float widthPx, float heightPx, float estimatedHeightPx);

    // Panels
    // The application and window it runs in (#1547). Never null.
    std::unique_ptr<IShellHost> m_Host;

    // The six panels and the window-independent wiring between them, shared with the PGO training
    // driver (#2136); named here as before.
    ShellPanels m_Panels;
    ProcessesPanel& m_ProcessesPanel = m_Panels.processes();
    ProcessDetailsPanel& m_ProcessDetailsPanel = m_Panels.processDetails();
    SystemMetricsPanel& m_SystemMetricsPanel = m_Panels.systemMetrics();
    ServicesPanel& m_ServicesPanel = m_Panels.services();
    StartupPanel& m_StartupPanel = m_Panels.startup();
    ConnectionsPanel& m_ConnectionsPanel = m_Panels.connections(); // #3066
    SystemInfoPanel& m_SystemInfoPanel = m_Panels.systemInfo();

    // Frame timing / FPS display
    FpsCounter m_FpsCounter;

    // Deferred startup settings: the first onUpdate() raises RefreshRateChangedEvent and
    // HistoryDurationChangedEvent with the loaded config, so panels get their starting values the
    // same way they get later changes (#1079).
    bool m_PendingStartupSettings = true;

    // Deferred startup notice: set in onAttach() when Settings wants the privilege notice. Decided and
    // dispatched in the first onUpdate() after the process probe's first generation (which says whether
    // privileges are reduced, #2731), with all layers stacked.
    bool m_PendingPrivilegeNotice = false;
    /// The privileged broker the access chip asks whether the session is held (#2728), resolved on the
    /// first update; null where there is none (no helper, a synthetic run) or off Linux.
    std::shared_ptr<Platform::IPrivilegedBroker> m_AccessBroker;
    bool m_AccessBrokerResolved = false;
    /// The window title last set (#2728): whether it says "(Administrator)", nullopt before the first.
    std::optional<bool> m_TitleSaysAdministrator;
    /// Whether the title and the access chip say Administrator (#2728): the helper is held for the session,
    /// or nothing is limited on a real machine. A synthetic run never is (#3004): its probe limits nothing
    /// because its machine is a stand-in, and its process actions are all refused.
    [[nodiscard]] bool isAdministratorSession(const Platform::ProcessCapabilities& capabilities) const;

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
    // The height without what the tabs measured, and the capped tab-independent minimum the window's
    // minimum was last set with: only a change in it may grow the window (minimumSizeChangeFor(), #1945).
    int m_ContentEstimatedHeightPx = 0;
    std::optional<WindowMinimumSize> m_GrowMinimumSize;
    TitleBarLayer* m_TitleBar = nullptr;

    // Render Metrics overlay (per-chart vertex count and CPU cost). Toggled with Ctrl+Shift+M; on from
    // the first frame with TASKSMACK_RENDER_METRICS set, and set by the demo script's metrics= step
    // (#2923). The status bar's FPS readout is shown only while it is on (#1200).
    bool m_ShowRenderMetrics = false;

    // The status bar's live text (#1200), rebuilt in onUpdate() only when the count or the interval
    // it was built from changes. The interval is the refresh rate from RefreshRateChangedEvent.
    int m_RefreshIntervalMs = 0;
    std::size_t m_StatusProcessCount = static_cast<std::size_t>(-1);
    int m_StatusIntervalMs = -1;
    std::string m_StatusProcessText;
    std::string m_StatusIntervalText;

    // Set by a ShowProcessDetailsEvent (#1209): the next tab bar selects the Process Details tab.
    bool m_ShowDetailsTabRequested = false;
    bool m_ShowServicesTabRequested = false; // A ShowServiceEvent (#1886): bring the Services tab forward
    // TASKSMACK_TAB (#1559): the tab the tab bar selects at startup, resolved once in onAttach() to the
    // tab's registered id and kept until that tab reports selected (#1575).
    SelectOverride::PendingMainTab m_StartupTab;
    // TASKSMACK_DEMO's tab= (#2827): the same, requested while running (requestMainTab()). Its own slot,
    // so startupSelectionPending() -- the screenshot hook's settle -- never waits for it.
    SelectOverride::PendingMainTab m_DemoTab;

    // Cached tab labels — rebuilt only when the underlying data changes, not every frame.
    // Avoids per-frame heap allocations from string concatenation in renderTabBar().
    // Both carry a fixed "###" ID suffix (TabLabel.h), so a new name never changes which tab ImGui
    // thinks is selected (#1140).
    // ICON + hostname: built in onAttach() ("System") and again when the System panel's first sample
    // brings the hostname (#2731), by updateSystemTabLabel(); m_TabBarMinimumLabels point into it, so
    // they are rebuilt with it.
    std::string m_CachedSystemTabLabel;
    std::string m_SystemTabLabelHostname; // The hostname m_CachedSystemTabLabel was built from
    bool m_SystemTabLabelBuilt = false;
    /// Rebuilds m_CachedSystemTabLabel and m_TabBarMinimumLabels when the hostname differs from the one
    /// the label shows; otherwise one string compare.
    void updateSystemTabLabel();
    TabLabel::CachedLabel m_DetailsTabLabel; // ICON + process name: rebuilt when the name changes
    // The Process Details tab with the program's own icon (#1800): spaces the icon is drawn over, then
    // the name. Rebuilt when the name, or the number of spaces the icon takes, changes.
    TabLabel::CachedLabel m_DetailsTabIconLabel;
    std::size_t m_DetailsTabIconSpaces = 0;
    ImTextureID m_DetailsTabIcon = ImTextureID_Invalid; // This frame's; ImTextureID_Invalid: the glyph label
    float m_DetailsTabIconSize = 0.0F;

    // The tab bar's items, rebuilt each frame into the same storage (no per-frame allocation once grown).
    std::vector<MainTabBarView::Tab> m_TabBarItems;
    // The tab labels the minimum window width covers (#1939), built with the system tab's label
    // (updateSystemTabLabel()): the Process Details tab's as it reads with no process selected, so the
    // minimum stays tab- and selection-independent.
    std::string m_DetailsTabNoSelectionLabel;
    std::vector<const char*> m_TabBarMinimumLabels;

    // Declared last so panels and cached labels outlive the non-owning registry.
    PanelTabs m_Tabs;
};

} // namespace App
