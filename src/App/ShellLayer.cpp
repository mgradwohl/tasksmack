#include "ShellLayer.h"

#include "AccessChoice.h"
#include "Core/ApplicationEvents.h"
#include "Core/EnvRead.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/Utf8Path.h"
#include "Core/WindowConstants.h"
#include "Core/WindowGeometry.h"
#include "Domain/ProcessSnapshot.h"
#include "FontSizeChange.h"
#include "KeyboardInput.h"
#include "KeyboardShortcuts.h"
#include "MainTabBarView.h"
#include "PanelPlatforms.h"
#include "Panels/ProcessDetailsPanel.h"
#include "Panels/ProcessIconView.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/ProcessTypes.h"
#include "RenderMetricsOverride.h"
#include "SelectOverride.h"
#include "ShellHost.h"
#include "ShellMetrics.h"
#include "StatusBarText.h"
#include "StatusBarView.h"
#include "SyntheticScenario.h"
#include "TabLabel.h"
#include "TitleBarGeometry.h"
#include "TitleBarLayer.h"
#include "UI/DpiScale.h"
#include "UI/Format.h"
#include "UI/HistoryPlotHeight.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UserConfig.h"
#include "WindowOverride.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{

namespace
{
// A literal, so the tab's label provider can hand it out with no storage of its own. Identified by its
// "###" suffix like the other main tabs (see TabLabel.h, #1140).
// Its visible text, unescaped, for TASKSMACK_TAB (#1559).
constexpr const char* PROCESSES_TAB_TEXT = "Processes";
constexpr const char* PROCESSES_TAB_LABEL = ICON_FA_LIST "  Processes###ProcessesTab";
static_assert(TabLabel::idPart(PROCESSES_TAB_LABEL) == TabLabel::PROCESSES_TAB_ID);
constexpr const char* SERVICES_TAB_TEXT = "Services";                                                 // #800
constexpr const char* SERVICES_TAB_LABEL = ICON_FA_GEARS "  Services###ServicesTab";                  // #800
constexpr const char* STARTUP_TAB_TEXT = "Startup";                                                   // #801
constexpr const char* STARTUP_TAB_LABEL = ICON_FA_POWER_OFF "  Startup###StartupTab";                 // #801
constexpr const char* CONNECTIONS_TAB_TEXT = "Connections";                                           // #3066
constexpr const char* CONNECTIONS_TAB_LABEL = ICON_FA_NETWORK_WIRED "  Connections###ConnectionsTab"; // #3066
// #1399. Last: static reference facts, visited least often, and the tabs before it keep their places.
constexpr const char* SYSTEM_INFO_TAB_TEXT = "System";
constexpr const char* SYSTEM_INFO_TAB_LABEL = ICON_FA_SERVER "  System###SystemInfoTab";

// The visible text follows the process's name, but the tab's ImGui ID does not: it comes from the
// fixed "###" suffix (see TabLabel.h). Hashed from the visible text, the ID changed whenever the name
// did -- an exec keeps the PID, so the selection survives it -- and ImGui, no longer finding the
// selected tab, switched to another one (#1140). A "#" in the name is shown as is (#1244).
[[nodiscard]] std::string makeDetailsTabLabel(std::string_view labelText)
{
    return TabLabel::make(ICON_FA_CIRCLE_INFO, labelText, TabLabel::PROCESS_DETAILS_TAB_ID);
}
} // namespace

ShellLayer::ShellLayer(std::unique_ptr<IShellHost> host)
    : Layer("ShellLayer"),
      m_Host(std::move(host)),
      m_Tabs({{.panel = m_SystemMetricsPanel,
               .eventName = "SystemOverview",
               .label = [this] { return m_CachedSystemTabLabel.c_str(); },
               .text = [this] { return std::string_view(m_SystemMetricsPanel.hostname()); }},
              {.panel = m_ProcessesPanel,
               .eventName = "Processes",
               .label = [] { return PROCESSES_TAB_LABEL; },
               .text = [] { return std::string_view(PROCESSES_TAB_TEXT); }},
              {.panel = m_ProcessDetailsPanel,
               .eventName = "ProcessDetails",
               .label = [this]
               { return (m_DetailsTabIcon != ImTextureID_Invalid ? m_DetailsTabIconLabel : m_DetailsTabLabel).label().c_str(); },
               .text = [this] { return std::string_view(m_ProcessDetailsPanel.tabLabel()); }},
              {.panel = m_ServicesPanel,
               .eventName = "Services",
               .label = [] { return SERVICES_TAB_LABEL; },
               .text = [] { return std::string_view(SERVICES_TAB_TEXT); }},
              {.panel = m_StartupPanel,
               .eventName = "Startup",
               .label = [] { return STARTUP_TAB_LABEL; },
               .text = [] { return std::string_view(STARTUP_TAB_TEXT); }},
              {.panel = m_ConnectionsPanel,
               .eventName = "Connections",
               .label = [] { return CONNECTIONS_TAB_LABEL; },
               .text = [] { return std::string_view(CONNECTIONS_TAB_TEXT); }},
              {.panel = m_SystemInfoPanel,
               .eventName = "SystemInfo",
               .label = [] { return SYSTEM_INFO_TAB_LABEL; },
               .text = [] { return std::string_view(SYSTEM_INFO_TAB_TEXT); }}})
{
    // One icon cache and texture set for every tab's program icons (#1800): ShellPanels hands the
    // Processes panel's to the Services and Startup tabs.
}

void ShellLayer::onAttach()
{
    spdlog::info("ShellLayer attached");

    // Load user configuration and apply to theme
    auto& config = UserConfig::get();
    config.load();
    config.applyToApplication();

    // The window may not be made smaller than the base minimum at this display's scale. Set here
    // because the shell is always present: TitleBarLayer widens the minimum to cover its own
    // content, but it is not created when native decorations are in use (#745), and without this
    // the window would have no SDL-level minimum at all in that mode. UILayer attaches first, so
    // the display scale is already known (#970).
    applyBaseMinimumWindowSize();

    // The recent crashes and hangs the System tab and Process Details share (#1675), before the panels
    // attach so the System tab's model is built with them.
    m_Panels.prepareAttach();

    // Initialize panels
    m_Tabs.onAttach();

    // The test hook's startup selection (#1559): read once here, applied when the process appears.
    // TASKSMACK_TAB wins over the Details tab the selection would bring forward.
    // An unknown TASKSMACK_TAB is ignored, with one warning, and changes nothing.
    const SelectOverride::MainTabChoice mainTab =
        SelectOverride::resolveMainTab(Core::readEnv(SelectOverride::MAIN_TAB_ENV_VAR), tabInfos());
    if (!mainTab.warning.empty())
    {
        spdlog::warn("{}", mainTab.warning);
    }
    m_StartupTab = SelectOverride::PendingMainTab(mainTab.id);
    if (const std::optional<SelectOverride::Target>& select = SelectOverride::active(); select.has_value())
    {
        m_ProcessesPanel.requestStartupSelection(select, SelectOverride::selectionShowsDetails(mainTab));
    }
    // TASKSMACK_MACHINE_TAB (#2869): the machine tab's sub-tab, asked for until it shows; an unknown
    // name is ignored with one warning. It picks no main tab: that stays TASKSMACK_TAB's, or the default.
    const SelectOverride::MachineTabChoice machineTab =
        SelectOverride::parseStartupMachineTab(Core::readEnv(SelectOverride::MACHINE_TAB_ENV_VAR));
    if (!machineTab.warning.empty())
    {
        spdlog::warn("{}", machineTab.warning);
    }
    if (machineTab.tab.has_value())
    {
        spdlog::info("{} (test hook): bringing the machine tab's {} sub-tab forward",
                     SelectOverride::MACHINE_TAB_ENV_VAR,
                     SelectOverride::machineTabName(*machineTab.tab));
        requestMachineTab(*machineTab.tab);
    }
    // TASKSMACK_RENDER_METRICS (#2923): the Render Metrics overlay from the first frame, as Ctrl+Shift+M
    // shows it; its CSV is written to the named file on detach, with the config. main() read and logged it.
    if (RenderMetricsOverride::active().has_value())
    {
        m_ShowRenderMetrics = true;
    }

    // Give ImGui back the Processes table's saved column layout before it is first drawn (#952).
    ProcessesPanel::restoreTableLayout(config.settings().processTableLayout);

    // Share the process model, the GPU model and the Services tab's list between the panels (#1880).
    m_Panels.connectModels();

    spdlog::info("Panels initialized");

    // Build stable tab labels. The system tab's carries the hostname, which the System panel learns
    // from its first sample, on its sampler thread (#2731): "System" now, the hostname once it is
    // adopted (updateSystemTabLabel() in onUpdate()), and unchanged for the rest of the process.
    m_DetailsTabLabel.get(m_ProcessDetailsPanel.tabLabel(), makeDetailsTabLabel);
    m_DetailsTabNoSelectionLabel = makeDetailsTabLabel(ProcessDetailsPanel::NO_SELECTION_TAB_TEXT);
    updateSystemTabLabel();

    // Trigger the startup notice if wanted. The status bar's lock icon reads the live value instead
    // (#1254); the notice itself is a one-off at startup. Whether the probe has reduced privileges is
    // known with its first generation, which the sampler thread is still building (#2731), so onUpdate()
    // decides once that has been adopted, after all layers are stacked (ElevationNoticeLayer hasn't been
    // pushed yet here).
    // Not while any test hook is set (#1559, #1569): the modal would cover the unattended capture,
    // and dismissing it needs input.
    m_PendingPrivilegeNotice = UserConfig::get().settings().showPrivilegeNotice && !SelectOverride::testHookActive();
}

bool ShellLayer::isAdministratorSession(const Platform::ProcessCapabilities& capabilities) const
{
    const bool held = m_AccessBroker != nullptr && m_AccessBroker->heldForSession();
    return held || (!capabilities.hasReducedPrivileges && Synthetic::activeScenario() == nullptr);
}

void ShellLayer::updateSystemTabLabel()
{
    const std::string& hostname = m_SystemMetricsPanel.hostname();
    if (m_SystemTabLabelBuilt && hostname == m_SystemTabLabelHostname)
    {
        return; // one string compare per frame, no allocation
    }
    m_CachedSystemTabLabel = TabLabel::make(ICON_FA_COMPUTER, hostname, TabLabel::SYSTEM_TAB_ID);
    m_SystemTabLabelHostname = hostname;
    m_SystemTabLabelBuilt = true;

    // The labels the window's minimum width is measured with (#1939): every tab's own, but the Process
    // Details tab's as it reads with nothing selected, so the minimum does not move with the selection.
    // A longer process name shrinks the tabs to fit instead. Rebuilt with the system tab's label, whose
    // storage they point into.
    m_TabBarMinimumLabels.clear();
    for (const auto& tab : m_Tabs.tabs())
    {
        m_TabBarMinimumLabels.push_back(tab.eventName == "ProcessDetails" ? m_DetailsTabNoSelectionLabel.c_str() : tab.label());
    }
}

std::vector<SelectOverride::TabInfo> ShellLayer::tabInfos() const
{
    std::vector<SelectOverride::TabInfo> infos;
    for (const auto& tab : m_Tabs.tabs())
    {
        infos.push_back({.id = tab.eventName, .text = tab.text ? tab.text() : std::string_view{}});
    }
    return infos;
}

bool ShellLayer::requestMainTab(const std::string_view name)
{
    // Resolved now, against the tabs as they are (the hostname tab's and the Process Details tab's
    // texts change), to the registered id the tab bar finds the tab by (#1575).
    const std::vector<SelectOverride::TabInfo> infos = tabInfos();
    const std::optional<std::size_t> index = SelectOverride::findTab(name, infos);
    if (!index.has_value())
    {
        return false;
    }
    m_DemoTab = SelectOverride::PendingMainTab(std::string(infos[*index].id));
    return true;
}

void ShellLayer::requestSelection(const SelectOverride::Target& target)
{
    m_ProcessesPanel.requestSelection(target);
}

void ShellLayer::requestDetailsTab(const SelectOverride::DetailsTab tab)
{
    m_ProcessDetailsPanel.requestTab(tab);
}

void ShellLayer::requestMachineTab(const SelectOverride::MachineTab tab)
{
    m_SystemMetricsPanel.requestTab(tab);
}

void ShellLayer::requestDetailsSection(const SelectOverride::DetailsSection section)
{
    m_ProcessDetailsPanel.requestSection(section);
}

void ShellLayer::requestRenderMetrics(const bool show) noexcept
{
    m_ShowRenderMetrics = show;
    // Synced now as well as in onUpdate(): a demo step is issued after this layer's onUpdate() ran, so
    // without this the charts of the frame the step lands on would not record.
    UI::RenderMetrics::get().setEnabled(show);
}

void ShellLayer::applyBaseMinimumWindowSize()
{
    m_MinimumSizeDisplayScale = UI::Theme::get().displayScale();
    m_MinimumSizeDisplayId = m_Host->displayId();
    if (m_Host->hasWindow())
    {
        // Held inside the display's usable bounds, so a large font on a small display cannot leave
        // a window that does not fit on-screen (#1207).
        const auto [usableWidth, usableHeight] = m_Host->usableDisplaySize().value_or(std::pair{0, 0});
        const auto minimumFor = [&](const int contentHeightPx)
        {
            return capMinimumToUsable(
                computeMinimumWindowSize(
                    m_MinimumSizeDisplayScale, 0.0F, static_cast<float>(m_ContentMinimumWidthPx), static_cast<float>(contentHeightPx)),
                usableWidth,
                usableHeight);
        };
        const WindowMinimumSize baseMinimum = minimumFor(m_ContentMinimumHeightPx);
        // Grows the window at startup and when the font, the scale or the display moved the minimum;
        // keeps its size when a tab's measurement alone did (#1945).
        const WindowMinimumSize growMinimum = minimumFor(m_ContentEstimatedHeightPx);
        const Core::WindowGeometry::MinimumSizeChange change = minimumSizeChangeFor(m_GrowMinimumSize, growMinimum);
        m_GrowMinimumSize = growMinimum;
        m_Host->setMinimumWindowSize(baseMinimum.width, baseMinimum.height, change);
    }
}

void ShellLayer::applyContentMinimumSize(float widthPx, float heightPx, float estimatedHeightPx)
{
    // With the custom title bar, it owns the minimum: hand it over, it re-derives the minimum every
    // frame and calls SDL only on a change. Otherwise apply it here when the whole-pixel width moves.
    if (m_TitleBar != nullptr)
    {
        m_TitleBar->setContentMinimumSize(widthPx, heightPx, estimatedHeightPx);
        return;
    }
    const auto toWholePx = [](const float px)
    {
        // NaN fails both comparisons in std::clamp and would pass straight through: treat it as unknown.
        return std::isfinite(px) ? static_cast<int>(std::ceil(std::clamp(px, 0.0F, static_cast<float>(Core::WINDOW_MAX_DIMENSION)))) : 0;
    };
    const int wholeWidth = toWholePx(widthPx);
    const int wholeHeight = toWholePx(heightPx);
    const int wholeEstimatedHeight = toWholePx(estimatedHeightPx);
    if (wholeWidth != m_ContentMinimumWidthPx || wholeHeight != m_ContentMinimumHeightPx ||
        wholeEstimatedHeight != m_ContentEstimatedHeightPx)
    {
        m_ContentMinimumWidthPx = wholeWidth;
        m_ContentMinimumHeightPx = wholeHeight;
        m_ContentEstimatedHeightPx = wholeEstimatedHeight;
        applyBaseMinimumWindowSize();
    }
}

void ShellLayer::onDetach()
{
    // Save user configuration
    auto& config = UserConfig::get();
    config.captureFromApplication();

    // Capture the window's normal (restored) geometry plus whether it is maximized. While it is
    // maximized its live size and position are the maximized ones; saving those made them the
    // restore target on the next launch, so Restore did nothing (#1121). When the normal rectangle
    // is unknown (maximized by the OS/compositor rather than by Window::maximize()), the geometry
    // saved last time is kept. The normal scale is, for a maximized window, the one captured with
    // its restore rectangle, not the live maximized one (#1168). With TASKSMACK_WINDOW set (#1453)
    // none of it is captured, so a measurement run leaves the saved geometry as it was.
    auto& settings = config.settings();
    WindowOverride::captureWindowGeometry(settings, m_Host->captureWindow(), WindowOverride::active());

    // Column widths, order and sort of the Processes table (#952). Empty means the table was never
    // drawn this session, so whatever was loaded is kept.
    if (std::string layout = m_ProcessesPanel.captureTableLayout(); !layout.empty())
    {
        settings.processTableLayout = std::move(layout);
    }

    // Stop the panels first: their samplers are torn down in order before the save, and anything
    // a panel writes back into the settings on detach is included in it (#1124, #1177).
    m_Tabs.onDetach();

    // Synchronous, as nothing draws after it. save() first waits for a background save still in
    // flight or queued (a click just before exit), then writes these settings over it: nothing is
    // lost, and the last state wins (#2916).
    config.save();

    // TASKSMACK_RENDER_METRICS (#2923): the overlay's CSV -- the last completed frame's rows, the bytes its
    // Copy CSV button puts on the clipboard -- to the named file, here with the config save: the one
    // place the UI thread writes a file, at exit, when no frame waits on it.
    if (const std::optional<std::filesystem::path>& csvPath = RenderMetricsOverride::active(); csvPath.has_value())
    {
        const std::string csv = UI::RenderMetrics::get().toCsv();
        if (const auto written = RenderMetricsOverride::writeCsv(*csvPath, csv); written.has_value())
        {
            spdlog::info("{}: Render Metrics CSV written: {} ({} rows)",
                         RenderMetricsOverride::ENV_VAR,
                         Core::pathToUtf8(*csvPath),
                         RenderMetricsOverride::csvRowCount(csv));
        }
        else
        {
            spdlog::error("{}: {}", RenderMetricsOverride::ENV_VAR, written.error());
        }
    }
    spdlog::info("ShellLayer detached");
}

void ShellLayer::onEvent(Core::Event& event)
{
    // A request to see a process's details, from the Processes table's row menu (#1209): the
    // selection itself travels as the ProcessSelectedEvent raised before it.
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::ShowProcessDetailsEvent>(
        [this](Core::ShowProcessDetailsEvent& /*e*/)
        {
            m_ShowDetailsTabRequested = true;
            return false;
        });
    // A request to see a service, from Process Details' Services section (#1886): the Services panel
    // selects it from the same event.
    dispatcher.dispatch<Core::ShowServiceEvent>(
        [this](Core::ShowServiceEvent& /*e*/)
        {
            m_ShowServicesTabRequested = true;
            return false;
        });
    // The status bar says how often TaskSmack updates (#1200): the refresh rate every sampler follows.
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            m_RefreshIntervalMs = e.getIntervalMs();
            return false;
        });

    // Forward events to all panels; each handles the settings events it needs itself
    m_Tabs.onEvent(event);
}

void ShellLayer::onSDLEvent(SDL_Event* event)
{
    // A display changed mode or its work area moved -- possibly the same display, at the same scale --
    // so re-read the usable bounds the native-decorated minimum is capped to on the next update, as
    // TitleBarLayer does for the borderless bar (#1207).
    if (event != nullptr && invalidatesUsableBounds(event->type))
    {
        m_MinimumSizeDisplayId = 0;
    }
}

void ShellLayer::onUpdate(float deltaTime)
{
    // Publish the loaded settings on the first update, after all layers are stacked, through the
    // same events the Settings dialog raises when they change (#1079). Panels start from the
    // SamplingConfig defaults and take their configured values from these.
    if (m_PendingStartupSettings)
    {
        m_PendingStartupSettings = false;
        ShellPanels::raiseStartupSettings([this](Core::Event& event) { m_Host->raiseEvent(event); });
    }

    // The access chip's broker, on the UI thread its prompt belongs to (#2728); Linux only, where
    // Administrator is the session-held helper.
    if (!m_AccessBrokerResolved)
    {
        m_AccessBrokerResolved = true;
        if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
        {
            m_AccessBroker = privilegedBroker(Synthetic::activeScenario());
        }
    }

    // "TaskSmack (Administrator)" while the session is (#2728), on Linux, whose access chip says the same;
    // set only when that changes.
    if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
    {
        if (m_ProcessesPanel.firstSnapshotAdopted())
        {
            const bool administrator = isAdministratorSession(m_ProcessesPanel.processCapabilities());
            if (m_TitleSaysAdministrator != administrator)
            {
                m_TitleSaysAdministrator = administrator;
                m_Host->setWindowTitle(administrator ? "TaskSmack (Administrator)" : "TaskSmack");
            }
        }
    }

    // Dispatch the startup privilege notice on the first update after the process probe's first
    // generation (#2731), with all layers stacked: that generation says whether the probe runs with
    // reduced privileges, which onAttach() could not know.
    if (m_PendingPrivilegeNotice && m_ProcessesPanel.firstSnapshotAdopted())
    {
        m_PendingPrivilegeNotice = false;
        if (const Platform::ProcessCapabilities capabilities = m_ProcessesPanel.processCapabilities(); capabilities.hasReducedPrivileges)
        {
            Core::OpenElevationNoticeEvent evt(capabilities.missingPrivileges);
            m_Host->raiseEvent(evt);
        }
    }

    // The frame's real duration, not deltaTime: deltaTime is capped at 0.1 s for animation, which made a
    // 6.7 FPS stall read "10.0 FPS (100.00 ms)" (#1152).
    m_FpsCounter.update(UI::Format::toFloatNarrow(m_Host->lastFrameIntervalSeconds()));

    // With native decorations, follow a display-scale change (#943), or a move to another display,
    // whose usable bounds the minimum is capped to (#1207). Not with the borderless title bar:
    // TitleBarLayer re-derives a wider minimum from the scale every frame, and re-applying the base
    // here would overwrite it.
    if (!m_Host->isBorderless() && (UI::displayScaleChanged(m_MinimumSizeDisplayScale, UI::Theme::get().displayScale()) ||
                                    m_Host->displayId() != m_MinimumSizeDisplayId))
    {
        applyBaseMinimumWindowSize();
    }

    // Update panels
    m_Tabs.onUpdate(deltaTime);

    // The hand-offs between the panels: capabilities, the selection count, Process Details' new
    // samples and the Services sampler's background demand (ShellPanels::update(), #2136).
    m_Panels.update(deltaTime);

    // The system tab's label takes the hostname when the System panel's first sample brings it (#2731).
    updateSystemTabLabel();

    // Rebuild the cached details tab label only when its text actually changes. Rebuilding on
    // every frame would allocate three std::string objects per frame at 60 fps; comparing the
    // panel's label against the cached text allocates nothing.
    //
    // Keyed on the text itself, not on the selection's identity. Identity is the wrong key: the
    // label also changes when nothing about the selection does -- most simply when the selected
    // process's first snapshot arrives a frame after the selection, which used to leave the tab
    // titled "Select a process" for as long as that process stayed selected.
    //
    // The cache builds the new label before it records the text it was built from, so a throw while
    // building leaves the old label and text together and the next frame tries again.
    m_DetailsTabLabel.get(m_ProcessDetailsPanel.tabLabel(), makeDetailsTabLabel);

    // Handle keyboard shortcuts for font size
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
    {
        // Theme steps to the next preset; changeFontSize() then saves it (#1076).
        auto& theme = UI::Theme::get();
        const bool grow = ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd);
        const bool shrink = ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract);
        if ((grow && theme.increaseFontSize()) || (shrink && theme.decreaseFontSize()))
        {
            changeFontSize(theme.currentFontSize());
        }
    }

    // The status bar's text (#1200), rebuilt only when what it says changes: the process count moves
    // once a sample at most, the interval only from Settings.
    // From the generation ProcessesPanel adopted this update: no lock in the render path.
    const std::size_t processCount = m_ProcessesPanel.adoptedProcessCount();
    if (processCount != m_StatusProcessCount)
    {
        // Each segment leads with an icon (#977); the fitting below measures the whole string, icon included.
        m_StatusProcessText = ICON_FA_LIST "  " + StatusBarText::processCountText(processCount);
        m_StatusProcessCount = processCount;
    }
    if (m_RefreshIntervalMs != m_StatusIntervalMs)
    {
        // No interval yet reads as no segment at all (empty), so the icon goes on only when there is text.
        const std::string intervalText = StatusBarText::updateIntervalText(m_RefreshIntervalMs);
        m_StatusIntervalText = intervalText.empty() ? std::string{} : ICON_FA_ARROWS_ROTATE "  " + intervalText;
        m_StatusIntervalMs = m_RefreshIntervalMs;
    }

    // Ctrl+Shift+M: toggle the Render Metrics overlay (per-chart vertex count and CPU cost)
    if (io.KeyCtrl && io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_M, false))
    {
        m_ShowRenderMetrics = !m_ShowRenderMetrics;
    }
    // Sync capture state before any charts render this frame; renderOverlay runs at the
    // end of onRender, so relying on it alone leaves capture one frame out of phase.
    UI::RenderMetrics::get().setEnabled(m_ShowRenderMetrics);
}

void ShellLayer::onRender()
{
    // Publish the previous frame's totals unconditionally, before any chart has a chance to
    // render this frame. Doing this here (not leaving it to record(), which only runs when a
    // chart actually renders) means a frame with zero chart activity still correctly publishes
    // an empty "last frame" instead of leaving the previous chart-bearing frame's totals in
    // place (see #875). Must run in onRender, not onUpdate: Application::run() calls onUpdate()
    // for every layer before onRender() for any layer, and ImGui::GetFrameCount() only advances
    // inside UILayer::onRender()'s call to ImGui::NewFrame() -- calling this from onUpdate reads
    // the *previous* frame's count, making it a no-op against the already-equal m_CurrentFrame
    // and silently reintroducing the exact bug this is meant to fix. This relies on UILayer
    // being pushed (and therefore rendered) before ShellLayer -- see main.cpp's pushLayer order.
    UI::RenderMetrics::get().beginFrame(ImGui::GetFrameCount());

    // The function keys, read once. F1, F2 and F10 act now; F5 and F9 depend on the tab on show, so
    // they wait for the tab bar to take this frame's tab click (#170).
    const KeyboardShortcuts::ShortcutAction shortcut = KeyboardInput::pollFunctionKeys();
    handleGlobalShortcut(shortcut);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Get dynamic title bar height (matches tab bars). Zero when native OS decorations are in
    // use instead of the custom title bar (see #745) -- TitleBarLayer isn't pushed in that case.
    const float titleBarHeight = m_Host->isBorderless() ? m_Host->titleBarHeight() : 0.0F;

    // Calculate status bar height
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    // Create fullscreen window that covers the viewport minus title bar and status bar
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + titleBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight - titleBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // Deliberately without ImGuiWindowFlags_NoSavedSettings. A table inherits that flag from its
    // top-level window, and with it ImGui neither records nor restores the table's column layout --
    // which is what stopped the Processes table's widths and order surviving a restart (#952).
    // Nothing of this window's own is persisted as a result: its position and size are set every
    // frame above, and ImGui's ini file is disabled, so its settings entry only ever lives in memory.
    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

    if (ImGui::Begin("##MainWindow", nullptr, windowFlags))
    {
        ImGui::PopStyleVar(3);

        renderTabBar();
        // After the tab bar, before the tabs draw: F5 and F9 go to the tab drawn this frame, and its
        // render takes the request in this same frame.
        handleTabShortcut(shortcut);
        // This window has no padding, so the cursor now sits exactly the main tab strip's height down.
        const float mainTabsHeight = ImGui::GetCursorPosY();

        // Render content area with padding. Authored at the reference configuration and scaled
        // like the style it overrides, so the gutter keeps its proportion to the text (#971).
        const float styleScale = UI::Theme::get().styleScale();
        const float contentPaddingH = ShellMetrics::CONTENT_PADDING_H * styleScale;
        const float contentPaddingV = ShellMetrics::CONTENT_PADDING_V * styleScale;

        // Add padding by using a child window with border that provides internal padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(contentPaddingH, contentPaddingV));

        // Let ImGui own child sizing so each panel can consume full available height without
        // shell-level scrollbar reservations that affect non-process tabs.
        if (ImGui::BeginChild("##ContentArea", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            m_Tabs.renderContent();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // The window may not be narrower than the panels' content: the Processes toolbar row, or the
        // Overview's NowBar column beside MIN_PLOT_WIDTH_EM of plot (#1207). Nor shorter than the
        // title bar, the tab strips, one chart at its minimum height and the status bar (#1278).
        // Measured with the body font, after the panels have drawn with it -- so the tab on show has
        // measured its first chart at this frame's width (#1370 review); a few text measurements a frame.
        const ImGuiStyle& style = ImGui::GetStyle();
        const float fontSize = ImGui::GetFontSize();
        const float plotMinHeight = std::floor(UI::Widgets::historyPlotMinHeight(fontSize, UI::chartEmPx()));
        // The tallest tab's lead-in and first chart, at its floor (UI/HistoryPlotHeight.h): estimated
        // from the style -- the tab bodies draw with the theme's FramePadding (TabContentScope), which
        // is what is pushed here, outside the tab bars -- or as the tabs last measured it, which also
        // counts a value strip wrapped onto extra rows at this width. Only measurements taken at this
        // window width and font size count: a hidden tab's goes stale when either changes (#1370 review).
        const float estimatedFirstChartBlock = computeTallestFirstChartBlock({
            .textLineWithSpacingPx = ImGui::GetTextLineHeightWithSpacing(),
            .frameHeightWithSpacingPx = ImGui::GetFrameHeightWithSpacing(),
            .itemSpacingYPx = style.ItemSpacing.y,
            .cellPaddingYPx = style.CellPadding.y,
            .plotMinHeightPx = plotMinHeight,
        });
        const auto contentHeightFor = [&](const float firstChartBudget)
        {
            return computeContentMinimumHeight({
                .titleBarPx = titleBarHeight,
                .mainTabsPx = mainTabsHeight,
                // As the panels draw them: a tab bar at SUB_TAB_PADDING_Y, then the item spacing below it.
                .subTabsPx = fontSize + (ShellMetrics::SUB_TAB_PADDING_Y * styleScale * 2.0F) + style.ItemSpacing.y,
                .chartPx = firstChartBudget,
                .statusBarPx = statusBarHeight,
                .chromePx = contentPaddingV * 2.0F,
            });
        };
        // Nor narrower than the main tab bar with every tab whole, inside its insets (#1939): measured
        // from fixed labels, so like the panels' width it moves only with the font and the scale.
        const float mainTabBarWidth = MainTabBarView::measureWidth(m_TabBarMinimumLabels);
        // Also from the estimate alone: the tab-independent height, whose changes (font, scale) may
        // grow the window, where a tab's measurement may not (#1945).
        applyContentMinimumSize(
            std::max(computeContentMinimumWidth(ProcessesPanel::measureToolbarMinimumWidth(),
                                                m_SystemMetricsPanel.overviewNowBarColumnWidth(),
                                                fontSize,
                                                (contentPaddingH * 2.0F) + style.ScrollbarSize),
                     mainTabBarWidth),
            contentHeightFor(computeFirstChartBudget(
                estimatedFirstChartBlock, m_SystemMetricsPanel.firstChartNonPlotHeight(viewport->Size.x, fontSize), plotMinHeight)),
            contentHeightFor(computeFirstChartBudget(estimatedFirstChartBlock, 0.0F, plotMinHeight)));
    }
    else
    {
        ImGui::PopStyleVar(3);
    }
    ImGui::End();

    renderStatusBar();

    UI::RenderMetrics::get().renderOverlay(&m_ShowRenderMetrics);

    // A shortcut request no render took this frame is dropped, never kept for a later one (#170).
    m_ProcessesPanel.expireFrameRequests();
    m_ProcessDetailsPanel.expireFrameRequests();
}

void ShellLayer::handleGlobalShortcut(KeyboardShortcuts::ShortcutAction action)
{
    // KeyboardInput::pollFunctionKeys() already returns None while a text field has the keyboard, a
    // popup or modal (Settings, About, a confirm, a menu) is open, or a modifier is held.
    using KeyboardShortcuts::ShortcutAction;
    switch (action)
    {
    case ShortcutAction::ShowHelp:
    {
        // The Help window (#172); About is reached from its footer and from Settings.
        Core::OpenHelpEvent event;
        m_Host->raiseEvent(event);
        break;
    }
    case ShortcutAction::OpenSettings:
    {
        Core::OpenSettingsEvent event;
        m_Host->raiseEvent(event);
        break;
    }
    case ShortcutAction::ToggleTreeView:
    case ShortcutAction::KillSelected:
        break; // Tab-sensitive: handleTabShortcut(), after the tab bar
    case ShortcutAction::Quit:
        // The title bar Close button's path: a close request, vetoable, and onDetach() saves settings.
        m_Host->requestClose();
        break;
    case ShortcutAction::None:
        break;
    }
}

void ShellLayer::handleTabShortcut(KeyboardShortcuts::ShortcutAction action)
{
    // F5 only where the table is on screen (a hidden view change would be a surprise later); F9 to the
    // panel showing the selection, which opens its own confirm dialog -- neither kills without the
    // dialog's own Kill button. The request is taken by that panel's render this frame.
    switch (KeyboardShortcuts::tabShortcutTarget(action, m_Tabs.activeTab().eventName))
    {
    case KeyboardShortcuts::ShortcutTarget::Processes:
        if (action == KeyboardShortcuts::ShortcutAction::ToggleTreeView)
        {
            m_ProcessesPanel.toggleTreeView();
        }
        else
        {
            m_ProcessesPanel.requestKillSelected();
        }
        break;
    case KeyboardShortcuts::ShortcutTarget::ProcessDetails:
        m_ProcessDetailsPanel.requestKillSelected();
        break;
    case KeyboardShortcuts::ShortcutTarget::None:
        break;
    }
}

void ShellLayer::updateDetailsTabIcon()
{
    m_DetailsTabIcon = ImTextureID_Invalid;
    ProcessIconView& icons = m_ProcessesPanel.iconView();
    icons.setEnabled(UserConfig::get().settings().showProcessIcons);
    const Domain::ProcessSnapshot* shown = m_ProcessDetailsPanel.displayedSnapshot();
    if (shown == nullptr || !icons.enabled())
    {
        return;
    }
    icons.beginFrame();
    const ImTextureID texture = icons.textureFor(*shown);
    // A program without an icon (or one still loading) keeps the tab's own glyph.
    const float spaceWidth = ImGui::CalcTextSize(" ").x;
    if (texture == ImTextureID_Invalid || spaceWidth <= 0.0F)
    {
        return;
    }
    // Spaces as wide as the icon at least: the label's text starts after them, and the icon is drawn
    // over them (MainTabBarView).
    const auto spaces = static_cast<std::size_t>(std::ceil(icons.iconSize() / spaceWidth));
    if (spaces != m_DetailsTabIconSpaces)
    {
        m_DetailsTabIconLabel = {};
        m_DetailsTabIconSpaces = spaces;
    }
    m_DetailsTabIconLabel.get(m_ProcessDetailsPanel.tabLabel(),
                              [spaces](std::string_view text)
                              { return TabLabel::make(std::string(spaces, ' '), text, TabLabel::PROCESS_DETAILS_TAB_ID); });
    m_DetailsTabIcon = texture;
    m_DetailsTabIconSize = icons.iconSize();
}

void ShellLayer::renderTabBar()
{
    // Track previous tab to emit change event if selection changes
    const auto* previousTab = &m_Tabs.activeTab();

    updateDetailsTabIcon();

    // TASKSMACK_TAB's tab (and TASKSMACK_DEMO's, #2827) is found by its registered id, not its position,
    // and asked for on every frame until BeginTabItem() reports it selected (#1575).
    m_TabBarItems.clear();
    for (const auto& tab : m_Tabs.tabs())
    {
        const bool details = tab.eventName == "ProcessDetails";
        m_TabBarItems.push_back({.label = tab.label(),
                                 .selectRequested = (m_ShowDetailsTabRequested && details) ||
                                                    (m_ShowServicesTabRequested && tab.eventName == "Services") ||
                                                    m_StartupTab.wantsSelected(tab.eventName) || m_DemoTab.wantsSelected(tab.eventName),
                                 .icon = details ? m_DetailsTabIcon : ImTextureID_Invalid,
                                 .iconSize = details ? m_DetailsTabIconSize : 0.0F});
    }
    const MainTabBarView::Result bar = MainTabBarView::render(m_TabBarItems);
    if (!bar.drawn)
    {
        return;
    }

    std::size_t index = 0;
    for (const auto& tab : m_Tabs.tabs())
    {
        m_StartupTab.onTabSubmitted(tab.eventName, bar.selected == index);
        m_DemoTab.onTabSubmitted(tab.eventName, bar.selected == index);
        ++index;
    }
    if (bar.selected.has_value())
    {
        m_Tabs.select(*bar.selected);
    }
    m_ShowDetailsTabRequested = false;
    m_ShowServicesTabRequested = false;
    if (const std::optional<std::string> dropped = m_StartupTab.onFrameEnd(); dropped.has_value())
    {
        spdlog::warn("{}: tab '{}' was not selected after {} frames; ignored",
                     SelectOverride::MAIN_TAB_ENV_VAR,
                     *dropped,
                     SelectOverride::MAX_MAIN_TAB_FRAMES);
    }
    if (const std::optional<std::string> dropped = m_DemoTab.onFrameEnd(); dropped.has_value())
    {
        spdlog::warn("Requested tab '{}' was not selected after {} frames; ignored", *dropped, SelectOverride::MAX_MAIN_TAB_FRAMES);
    }

    // Emit ActiveTabChangedEvent when tab selection changes
    if (previousTab != &m_Tabs.activeTab())
    {
        Core::ActiveTabChangedEvent evt(m_Tabs.activeTab().eventName);
        m_Host->raiseEvent(evt);
    }
}

void ShellLayer::renderStatusBar() const
{
    // Live, not a startup copy: a probe can withdraw a capability after the first sample (#1254).
    // ProcessesPanel keeps it with its cached snapshot generation, so reading it takes no lock.
    const Platform::ProcessCapabilities capabilities = m_ProcessesPanel.processCapabilities();
    // When native OS decorations replace the custom title bar (see #745), its Settings/Help buttons
    // don't exist, so the status bar carries them.
    // Standard or Administrator (#2728), on Linux: Administrator when nothing is limited (root, or every
    // capability granted) or the helper is held for the session; Windows keeps the plain lock until
    // TaskSmack can restart itself elevated.
    std::optional<StatusBarView::AccessChip> access;
    if constexpr (AccessChoice::HOST == AccessChoice::AccessPlatform::Linux)
    {
        access = isAdministratorSession(capabilities) ? StatusBarView::AccessChip::Administrator : StatusBarView::AccessChip::Standard;
    }
    const StatusBarView::Action action = StatusBarView::render({
        .access = access,
        .reducedPrivileges = capabilities.hasReducedPrivileges,
        .networkCountersBlocked = capabilities.networkCountersBlocked,
        .processText = m_StatusProcessText,
        .intervalText = m_StatusIntervalText,
        .showControls = !m_Host->isBorderless(),
        .fps = m_ShowRenderMetrics ? std::optional<float>(m_FpsCounter.displayedFps()) : std::nullopt,
        .frameTimeSeconds = m_FpsCounter.displayedFrameTime(),
    });
    if (action == StatusBarView::Action::Settings)
    {
        Core::OpenSettingsEvent event;
        m_Host->raiseEvent(event);
    }
    else if (action == StatusBarView::Action::Help)
    {
        Core::OpenHelpEvent event;
        m_Host->raiseEvent(event);
    }
    else if (action == StatusBarView::Action::Access)
    {
        // The startup notice again, whatever its "Don't show this again": the user asked for it.
        Core::OpenElevationNoticeEvent event(capabilities.missingPrivileges);
        m_Host->raiseEvent(event);
    }
}

} // namespace App
