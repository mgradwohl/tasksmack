#include "ShellLayer.h"

#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Core/WindowConstants.h"
#include "Domain/ProcessSnapshot.h"
#include "FontSizeChange.h"
#include "Panels/ProcessesPanel.h"
#include "Panels/SystemMetricsPanel.h"
#include "Platform/ProcessTypes.h"
#include "ShellMetrics.h"
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
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace App
{

namespace
{
// A literal, so the tab's label provider can hand it out with no storage of its own. Identified by its
// "###" suffix like the other main tabs (see TabLabel.h, #1140).
constexpr const char* PROCESSES_TAB_LABEL = ICON_FA_LIST "  Processes###ProcessesTab";
static_assert(TabLabel::idPart(PROCESSES_TAB_LABEL) == TabLabel::PROCESSES_TAB_ID);

// The visible text follows the process's name, but the tab's ImGui ID does not: it comes from the
// fixed "###" suffix (see TabLabel.h). Hashed from the visible text, the ID changed whenever the name
// did -- an exec keeps the PID, so the selection survives it -- and ImGui, no longer finding the
// selected tab, switched to another one (#1140). A "#" in the name is shown as is (#1244).
[[nodiscard]] std::string makeDetailsTabLabel(std::string_view labelText)
{
    return TabLabel::make(ICON_FA_CIRCLE_INFO, labelText, TabLabel::PROCESS_DETAILS_TAB_ID);
}
} // namespace

ShellLayer::ShellLayer()
    : Layer("ShellLayer"),
      m_Tabs(
          {{.panel = m_SystemMetricsPanel, .eventName = "SystemOverview", .label = [this] { return m_CachedSystemTabLabel.c_str(); }},
           {.panel = m_ProcessesPanel, .eventName = "Processes", .label = [] { return PROCESSES_TAB_LABEL; }},
           {.panel = m_ProcessDetailsPanel, .eventName = "ProcessDetails", .label = [this] { return m_DetailsTabLabel.label().c_str(); }}})
{}

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

    // Initialize panels
    m_Tabs.onAttach();

    // Give ImGui back the Processes table's saved column layout before it is first drawn (#952).
    ProcessesPanel::restoreTableLayout(config.settings().processTableLayout);

    // Share the process model with panels that render system-level aggregates
    if (const auto processModel = m_ProcessesPanel.processModel(); processModel != nullptr)
    {
        m_SystemMetricsPanel.setProcessModel(processModel);

        // Share GPU model with ProcessModel for per-process GPU data
        if (auto gpuModel = m_SystemMetricsPanel.gpuModel(); gpuModel != nullptr)
        {
            processModel->setGPUModel(gpuModel);
            // Its probe also decides whether the GPU columns can be filled at all (#1210).
            m_ProcessesPanel.setGpuModel(gpuModel);
        }
    }

    spdlog::info("Panels initialized");

    // Build stable tab labels. Hostname doesn't change for the process lifetime,
    // so the system tab label is built once here.
    m_CachedSystemTabLabel = TabLabel::make(ICON_FA_COMPUTER, m_SystemMetricsPanel.hostname(), TabLabel::SYSTEM_TAB_ID);
    m_DetailsTabLabel.get(m_ProcessDetailsPanel.tabLabel(), makeDetailsTabLabel);

    // Trigger the startup notice if needed. The status bar's lock icon reads the live value instead
    // (#1254); the notice itself is a one-off at startup.
    // NOTE: The event is NOT dispatched here — ElevationNoticeLayer hasn't been pushed yet.
    // m_PendingPrivilegeNotice is dispatched in the first onUpdate() call, after all layers are stacked.
    if (m_ProcessesPanel.hasReducedPrivileges() && UserConfig::get().settings().showPrivilegeNotice)
    {
        m_PendingPrivilegeNotice = true;
    }

    // The details pane draws only the series the process probe can fill (#1028, #1035). Refreshed
    // every update too, since a probe can withdraw a capability after the first sample (#1254).
    m_ProcessDetailsPanel.setProcessCapabilities(m_ProcessesPanel.processCapabilities());
}

void ShellLayer::applyBaseMinimumWindowSize()
{
    m_MinimumSizeDisplayScale = UI::Theme::get().displayScale();
    const auto& window = Core::Application::get().getWindow();
    m_MinimumSizeDisplayId = window.getDisplayId();
    if (SDL_Window* sdlWindow = window.getHandle(); sdlWindow != nullptr)
    {
        // Held inside the display's usable bounds, so a large font on a small display cannot leave
        // a window that does not fit on-screen (#1207).
        const auto [usableWidth, usableHeight] = window.getUsableDisplaySize().value_or(std::pair{0, 0});
        const WindowMinimumSize baseMinimum = capMinimumToUsable(
            computeMinimumWindowSize(
                m_MinimumSizeDisplayScale, 0.0F, static_cast<float>(m_ContentMinimumWidthPx), static_cast<float>(m_ContentMinimumHeightPx)),
            usableWidth,
            usableHeight);
        if (!SDL_SetWindowMinimumSize(sdlWindow, baseMinimum.width, baseMinimum.height))
        {
            spdlog::warn("SDL_SetWindowMinimumSize({}, {}) failed: {}", baseMinimum.width, baseMinimum.height, SDL_GetError());
        }
    }
}

void ShellLayer::applyContentMinimumSize(float widthPx, float heightPx)
{
    // With the custom title bar, it owns the minimum: hand it over, it re-derives the minimum every
    // frame and calls SDL only on a change. Otherwise apply it here when the whole-pixel width moves.
    if (m_TitleBar != nullptr)
    {
        m_TitleBar->setContentMinimumSize(widthPx, heightPx);
        return;
    }
    const auto toWholePx = [](const float px)
    {
        // NaN fails both comparisons in std::clamp and would pass straight through: treat it as unknown.
        return std::isfinite(px) ? static_cast<int>(std::ceil(std::clamp(px, 0.0F, static_cast<float>(Core::WINDOW_MAX_DIMENSION)))) : 0;
    };
    const int wholeWidth = toWholePx(widthPx);
    const int wholeHeight = toWholePx(heightPx);
    if (wholeWidth != m_ContentMinimumWidthPx || wholeHeight != m_ContentMinimumHeightPx)
    {
        m_ContentMinimumWidthPx = wholeWidth;
        m_ContentMinimumHeightPx = wholeHeight;
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
    const auto& window = Core::Application::get().getWindow();
    auto& settings = config.settings();
    const WindowOverride::CapturedWindow captured{
        .normal = window.getNormalGeometry(),
        .normalScale = window.getNormalGeometryScale(),
        .canPosition = Core::Window::supportsPositioning(),
        .maximized = window.isMaximized(),
    };
    WindowOverride::captureWindowGeometry(settings, captured, WindowOverride::active());

    // Column widths, order and sort of the Processes table (#952). Empty means the table was never
    // drawn this session, so whatever was loaded is kept.
    if (std::string layout = m_ProcessesPanel.captureTableLayout(); !layout.empty())
    {
        settings.processTableLayout = std::move(layout);
    }

    // Stop the panels first: their samplers are torn down in order before the save, and anything
    // a panel writes back into the settings on detach is included in it (#1124, #1177).
    m_Tabs.onDetach();

    config.save();
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
        const auto& settings = UserConfig::get().settings();
        // The synthetic scenario (#1413) may start at its own window and interval, for this run only.
        const Synthetic::Scenario* scenario = Synthetic::activeScenario();
        Core::RefreshRateChangedEvent refreshEvent(Synthetic::startupRefreshIntervalMs(scenario, settings.refreshIntervalMs),
                                                   /*initial=*/true);
        Core::Application::get().raiseEvent(refreshEvent);
        Core::HistoryDurationChangedEvent historyEvent(Synthetic::startupHistorySeconds(scenario, settings.maxHistorySeconds),
                                                       /*initial=*/true);
        Core::Application::get().raiseEvent(historyEvent);
    }

    // Dispatch the startup privilege notice on the first update, after all layers are stacked.
    if (m_PendingPrivilegeNotice)
    {
        m_PendingPrivilegeNotice = false;
        Core::OpenElevationNoticeEvent evt;
        Core::Application::get().raiseEvent(evt);
    }

    // The frame's real duration, not deltaTime: deltaTime is capped at 0.1 s for animation, which made a
    // 6.7 FPS stall read "10.0 FPS (100.00 ms)" (#1152).
    m_FpsCounter.update(UI::Format::toFloatNarrow(Core::Application::get().lastFrameIntervalSeconds()));

    // With native decorations, follow a display-scale change (#943), or a move to another display,
    // whose usable bounds the minimum is capped to (#1207). Not with the borderless title bar:
    // TitleBarLayer re-derives a wider minimum from the scale every frame, and re-applying the base
    // here would overwrite it.
    if (const auto& window = Core::Application::get().getWindow();
        !window.isBorderless() && (UI::displayScaleChanged(m_MinimumSizeDisplayScale, UI::Theme::get().displayScale()) ||
                                   window.getDisplayId() != m_MinimumSizeDisplayId))
    {
        applyBaseMinimumWindowSize();
    }

    // Update panels
    m_Tabs.onUpdate(deltaTime);

    // The details pane follows the capabilities published with the latest generation (#1254): a
    // plain copy of what ProcessesPanel fetched with its snapshots, so no lock is taken here.
    m_ProcessDetailsPanel.setProcessCapabilities(m_ProcessesPanel.processCapabilities());
    // Per-process GPU support reaches the details pane with each sample, as of the generation it
    // came from (Domain::ProcessSample, #1210), not from here.

    // Hand Process Details the selected process's new samples: one per generation the sampler
    // published since its last frame, each with its own sample time (#1098). The model keeps them for
    // the watched PID, so a frame with nothing new costs one atomic load and copies nothing -- this
    // used to look the process up and deep-copy its snapshot twice every frame (#1172).
    const std::int32_t selectedPid = m_ProcessDetailsPanel.selectedPid();
    if (selectedPid != m_WatchedPid)
    {
        m_ProcessesPanel.watchProcess(selectedPid);
        m_WatchedPid = selectedPid;
    }
    m_PendingSamples.clear();
    if (selectedPid != -1)
    {
        static_cast<void>(m_ProcessesPanel.watchedSamplesSince(m_ProcessDetailsPanel.lastSampleVersion(), m_PendingSamples));
    }
    m_ProcessDetailsPanel.updateWithSamples(m_PendingSamples, deltaTime);

    // Debug: Log when GPU data becomes available for the selected PID.
    // This avoids spamming logs every frame while a GPU-using process is selected.
    if (const Domain::ProcessSnapshot* selected = m_ProcessDetailsPanel.displayedSnapshot();
        selected != nullptr && !m_PendingSamples.empty())
    {
        const bool hasGpuData = (!selected->gpuDevices.empty() || (selected->gpuMemoryBytes > 0));
        if ((selectedPid != m_LastGpuLogPid) || !m_LastGpuLogHasData)
        {
            if (hasGpuData)
            {
                spdlog::debug("ShellLayer: Selected PID {} has GPU data: devices='{}', mem={}",
                              selectedPid,
                              selected->gpuDevices,
                              selected->gpuMemoryBytes);
            }

            m_LastGpuLogPid = selectedPid;
            m_LastGpuLogHasData = hasGpuData;
        }
    }
    // Drop this frame's references now: the panel keeps the one it shows.
    m_PendingSamples.clear();

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

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Get dynamic title bar height (matches tab bars). Zero when native OS decorations are in
    // use instead of the custom title bar (see #745) -- TitleBarLayer isn't pushed in that case.
    const float titleBarHeight = Core::Application::get().getWindow().isBorderless() ? TitleBarLayer::height() : 0.0F;

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
        const float firstChartBudget = computeFirstChartBudget(computeTallestFirstChartBlock({
                                                                   .textLineWithSpacingPx = ImGui::GetTextLineHeightWithSpacing(),
                                                                   .frameHeightWithSpacingPx = ImGui::GetFrameHeightWithSpacing(),
                                                                   .itemSpacingYPx = style.ItemSpacing.y,
                                                                   .cellPaddingYPx = style.CellPadding.y,
                                                                   .plotMinHeightPx = plotMinHeight,
                                                               }),
                                                               m_SystemMetricsPanel.firstChartNonPlotHeight(viewport->Size.x, fontSize),
                                                               plotMinHeight);
        applyContentMinimumSize(computeContentMinimumWidth(ProcessesPanel::measureToolbarMinimumWidth(),
                                                           m_SystemMetricsPanel.overviewNowBarColumnWidth(),
                                                           fontSize,
                                                           (contentPaddingH * 2.0F) + style.ScrollbarSize),
                                computeContentMinimumHeight({
                                    .titleBarPx = titleBarHeight,
                                    .mainTabsPx = mainTabsHeight,
                                    // As the panels draw them: a tab bar at SUB_TAB_PADDING_Y, then the item spacing below it.
                                    .subTabsPx = fontSize + (ShellMetrics::SUB_TAB_PADDING_Y * styleScale * 2.0F) + style.ItemSpacing.y,
                                    .chartPx = firstChartBudget,
                                    .statusBarPx = statusBarHeight,
                                    .chromePx = contentPaddingV * 2.0F,
                                }));
    }
    else
    {
        ImGui::PopStyleVar(3);
    }
    ImGui::End();

    renderStatusBar();

    UI::RenderMetrics::get().renderOverlay(&m_ShowRenderMetrics);
}

void ShellLayer::renderTabBar()
{
    // Every size here is authored at the reference configuration and scaled like the ImGuiStyle
    // values around it. As pixel literals pushed over a scaled style they kept one size while the
    // text they frame grew and shrank: the tabs' padding was more than a line of text at Small and
    // about half a line at Even Huger (#971).
    const float styleScale = UI::Theme::get().styleScale();

    // Add top edge padding for visual balance
    ImGui::Dummy(ImVec2(0.0F, ShellMetrics::TOP_EDGE_PADDING * styleScale));

    // Add left indent to align main tabs with panel tabs below (same as the content area's gutter)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (ShellMetrics::CONTENT_PADDING_H * styleScale));

    // Add horizontal padding inside tabs and vertical padding for taller tabs
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(ShellMetrics::TAB_PADDING_X * styleScale, ShellMetrics::MAIN_TAB_PADDING_Y * styleScale));

    if (ImGui::BeginTabBar("##MainTabBar",
                           ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip |
                               ImGuiTabBarFlags_DrawSelectedOverline))
    {
        // Track previous tab to emit change event if selection changes
        const auto* previousTab = &m_Tabs.activeTab();

        std::size_t index = 0;
        for (const auto& tab : m_Tabs.tabs())
        {
            ImGuiTabItemFlags tabFlags = ImGuiTabItemFlags_NoCloseWithMiddleMouseButton;
            if (m_ShowDetailsTabRequested && tab.eventName == "ProcessDetails")
            {
                tabFlags |= ImGuiTabItemFlags_SetSelected;
            }
            if (ImGui::BeginTabItem(tab.label(), nullptr, tabFlags))
            {
                m_Tabs.select(index);
                ImGui::EndTabItem();
            }
            ++index;
        }
        m_ShowDetailsTabRequested = false;

        ImGui::EndTabBar();

        // Emit ActiveTabChangedEvent when tab selection changes
        if (previousTab != &m_Tabs.activeTab())
        {
            Core::ActiveTabChangedEvent evt(m_Tabs.activeTab().eventName);
            Core::Application::get().raiseEvent(evt);
        }
    }

    ImGui::PopStyleVar();
}

void ShellLayer::renderStatusBar() const
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Calculate height dynamically based on font size for proper scaling
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    // When native OS decorations replace the custom title bar (see #745), TitleBarLayer is
    // skipped and this status bar becomes the only place Settings/Help can be reached -- keep
    // it keyboard-navigable in that mode so those buttons stay reachable without a mouse.
    const bool showStatusBarControls = !Core::Application::get().getWindow().isBorderless();
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!showStatusBarControls)
    {
        windowFlags |= ImGuiWindowFlags_NoNav;
    }

    // Use theme colors for status bar
    const auto& theme = UI::Theme::get();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.scheme().statusBarBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F); // Show top border
    // Center text vertically within the status bar
    const float verticalPadding = (statusBarHeight - ImGui::GetFontSize()) * 0.5F;
    const float statusBarPaddingX = ShellMetrics::STATUS_BAR_PADDING_X * UI::Theme::get().styleScale();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(statusBarPaddingX, verticalPadding));

    if (ImGui::Begin("##StatusBar", nullptr, windowFlags))
    {
        // Show a persistent lock icon when running without elevated privileges
        // Live, not a startup copy: a probe can withdraw a capability after the first sample (#1254).
        // ProcessesPanel keeps it with its cached snapshot generation, so reading it takes no lock.
        const Platform::ProcessCapabilities capabilities = m_ProcessesPanel.processCapabilities();
        if (capabilities.hasReducedPrivileges)
        {
            ImGui::TextColored(theme.scheme().textWarning, ICON_FA_LOCK);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Limited data: some details of other users' processes are unavailable");
            }
            ImGui::SameLine();
        }
        // Per-process network counters denied although TaskSmack already has the rights they need
        // (#1358): the network columns are gone, and running as Administrator would not bring them
        // back, so this is not the lock icon's "limited data" notice.
        if (capabilities.networkCountersBlocked)
        {
            ImGui::TextColored(theme.scheme().textWarning, ICON_FA_NETWORK_WIRED);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Per-process network counters are blocked on this system "
                                  "(TCP EStats access denied even when elevated)");
            }
            ImGui::SameLine();
        }

        ImGui::Text("Ready");

        // When native OS decorations replace the custom title bar (see #745), its
        // Settings/Help buttons don't exist -- surface equivalents here so those stay
        // reachable. Hidden otherwise since the title bar already provides them.
        if (showStatusBarControls)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_GEAR "##StatusBarSettings"))
            {
                Core::OpenSettingsEvent event;
                Core::Application::get().raiseEvent(event);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Settings");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_CIRCLE_QUESTION "##StatusBarHelp"))
            {
                Core::OpenAboutEvent event;
                Core::Application::get().raiseEvent(event);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("About / Help");
            }
        }

        // Where the left-hand content ends, window-local, for the FPS readout's fit check.
        const float leftContentEndX = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

        // Right-align FPS display. The text is formatted first and then measured, so the readout
        // ends at the status bar's padding at any font and any value. It used to be positioned from
        // the width of the *format string* plus a fixed 50px, which is not the width of what is
        // drawn (#971).
        std::array<char, 48> fpsText{};
        const auto fpsEnd = std::format_to_n(fpsText.data(),
                                             fpsText.size() - 1,
                                             "{:.1f} FPS ({:.2f} ms)",
                                             static_cast<double>(m_FpsCounter.displayedFps()),
                                             static_cast<double>(m_FpsCounter.displayedFrameTime() * 1000.0F));
        const float fpsWidth = ImGui::CalcTextSize(fpsText.data(), fpsEnd.out).x;
        const float fpsX = ImGui::GetWindowWidth() - statusBarPaddingX - fpsWidth;
        // Left out, not drawn over "Ready" and the buttons, when the window is too narrow (#1207).
        if (computeStatusBarReadoutFits(leftContentEndX, fpsX, ImGui::GetStyle().ItemSpacing.x))
        {
            ImGui::SameLine(fpsX);
            ImGui::TextUnformatted(fpsText.data(), fpsEnd.out);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace App
