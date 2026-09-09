#include "ShellLayer.h"

#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "TitleBarLayer.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/RenderMetrics.h"
#include "UI/Theme.h"
#include "UserConfig.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace App
{

ShellLayer::ShellLayer()
    : Layer("ShellLayer"),
      m_Tabs({{.panel = m_SystemMetricsPanel, .eventName = "SystemOverview", .label = [this] { return m_CachedSystemTabLabel.c_str(); }},
              {.panel = m_ProcessesPanel, .eventName = "Processes", .label = [] { return ICON_FA_LIST "  Processes"; }},
              {.panel = m_ProcessDetailsPanel, .eventName = "ProcessDetails", .label = [this] { return m_CachedDetailsTabLabel.c_str(); }}})
{}

void ShellLayer::onAttach()
{
    spdlog::info("ShellLayer attached");

    // Load user configuration and apply to theme
    auto& config = UserConfig::get();
    config.load();
    config.applyToApplication();

    // Initialize panels
    m_Tabs.onAttach();

    // Share the process model with panels that render system-level aggregates
    if (auto* processModel = m_ProcessesPanel.processModel(); processModel != nullptr)
    {
        m_SystemMetricsPanel.setProcessModel(processModel);

        // Share GPU model with ProcessModel for per-process GPU data
        if (auto gpuModel = m_SystemMetricsPanel.gpuModel(); gpuModel != nullptr)
        {
            processModel->setGPUModel(gpuModel);
        }
    }

    spdlog::info("Panels initialized");

    // Build stable tab labels. Hostname doesn't change for the process lifetime,
    // so the system tab label is built once here.
    m_CachedSystemTabLabel = std::string(ICON_FA_COMPUTER) + "  " + m_SystemMetricsPanel.hostname();
    m_CachedDetailsTabLabel = std::string(ICON_FA_CIRCLE_INFO) + "  Select a process";
    m_CachedLabelPid = -1;

    // Cache privilege status and trigger the startup notice if needed.
    // Elevation state is constant for process lifetime; cache once at startup.
    // NOTE: The event is NOT dispatched here — ElevationNoticeLayer hasn't been pushed yet.
    // m_PendingPrivilegeNotice is dispatched in the first onUpdate() call, after all layers are stacked.
    m_HasReducedPrivileges = m_ProcessesPanel.hasReducedPrivileges();
    if (m_HasReducedPrivileges && UserConfig::get().settings().showPrivilegeNotice)
    {
        m_PendingPrivilegeNotice = true;
    }
}

void ShellLayer::onDetach()
{
    // Save user configuration
    auto& config = UserConfig::get();
    config.captureFromApplication();

    // Capture current window geometry/state.
    auto& window = Core::Application::get().getWindow();
    const auto [width, height] = window.getSize();
    auto& settings = config.settings();
    settings.windowWidth = width;
    settings.windowHeight = height;

    if (Core::Window::supportsPositioning())
    {
        const auto [x, y] = window.getPosition();
        settings.windowPosX = x;
        settings.windowPosY = y;
    }

    settings.windowMaximized = window.isMaximized();

    config.save();

    m_Tabs.onDetach();
    spdlog::info("ShellLayer detached");
}

void ShellLayer::onEvent(Core::Event& event)
{
    // Handle app-wide coordination events
    Core::EventDispatcher dispatcher(event);
    dispatcher.dispatch<Core::RefreshRateChangedEvent>(
        [this](Core::RefreshRateChangedEvent& e)
        {
            const auto interval = std::chrono::milliseconds(e.getIntervalMs());
            m_ProcessesPanel.setSamplingInterval(interval);
            m_SystemMetricsPanel.setSamplingInterval(interval);
            return false; // Do not consume; allow others to react as well
        });

    // Forward events to all panels
    m_Tabs.onEvent(event);
}

void ShellLayer::onUpdate(float deltaTime)
{
    // Dispatch the startup privilege notice on the first update, after all layers are stacked.
    if (m_PendingPrivilegeNotice)
    {
        m_PendingPrivilegeNotice = false;
        Core::OpenElevationNoticeEvent evt;
        Core::Application::get().raiseEvent(evt);
    }

    m_FpsCounter.update(deltaTime);

    // Update panels
    m_Tabs.onUpdate(deltaTime);

    // Find the selected process snapshot for rendering
    // Note: Selection is now coordinated via ProcessSelectedEvent, but we still need
    // to look up the snapshot for ProcessDetailsPanel to render
    const Domain::ProcessSnapshot* selectedSnapshot = nullptr;
    Domain::ProcessSnapshot cachedSnapshot;
    std::uint64_t selectedSnapshotVersion = 0;
    const std::int32_t selectedPid = m_ProcessesPanel.selectedPid();
    if (selectedPid != -1)
    {
        // findSnapshotWithVersion() copies only the one matching entry out of ProcessModel (not
        // the full 200+ entry vector) and returns it together with the exact publication
        // version it was read under, atomically. The previous version of this code instead
        // paired findSnapshot() with a *separately*-read version from ProcessesPanel's own
        // render cache (which only refreshes while the Processes tab is active), which could
        // race with an intervening ProcessModel publish: findSnapshot() always reflects the
        // truly latest data, so while viewing Process Details on its own, the version passed
        // to updateWithSnapshot() below could stay stuck even as the snapshot content kept
        // changing, silently freezing history recording (ProcessDetailsPanel gates "is this
        // new data" on that version) while the live displayed values kept updating from the
        // snapshot itself.
        if (auto found = m_ProcessesPanel.findSnapshotWithVersion(selectedPid))
        {
            cachedSnapshot = std::move(found->snapshot);
            selectedSnapshotVersion = found->version;
            selectedSnapshot = &cachedSnapshot;

            // Debug: Log when GPU data becomes available for the selected PID.
            // This avoids spamming logs every frame while a GPU-using process is selected.
            const bool hasGpuData = (!cachedSnapshot.gpuDevices.empty() || (cachedSnapshot.gpuMemoryBytes > 0));

            if ((selectedPid != m_LastGpuLogPid) || !m_LastGpuLogHasData)
            {
                if (hasGpuData)
                {
                    spdlog::debug("ShellLayer: Selected PID {} has GPU data: devices='{}', mem={}",
                                  selectedPid,
                                  cachedSnapshot.gpuDevices,
                                  cachedSnapshot.gpuMemoryBytes);
                }

                m_LastGpuLogPid = selectedPid;
                m_LastGpuLogHasData = hasGpuData;
            }
        }
    }
    m_ProcessDetailsPanel.updateWithSnapshot(selectedSnapshot, selectedSnapshotVersion, deltaTime);

    // Update the cached details tab label only when the selected process changes.
    // Rebuilding on every frame would allocate three std::string objects per frame at 60 fps.
    if (selectedPid != m_CachedLabelPid)
    {
        m_CachedLabelPid = selectedPid;
        m_CachedDetailsTabLabel = std::string(ICON_FA_CIRCLE_INFO) + "  " + m_ProcessDetailsPanel.tabLabel();
    }

    // Handle keyboard shortcuts for font size
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
        {
            UI::Theme::get().increaseFontSize();
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
        {
            UI::Theme::get().decreaseFontSize();
        }
    }

    // Ctrl+Shift+M: toggle the Render Metrics overlay (per-chart vertex/index/CPU cost)
    if (io.KeyCtrl && io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_M, false))
    {
        m_ShowRenderMetrics = !m_ShowRenderMetrics;
    }
    // Sync capture state before any charts render this frame; renderOverlay runs at the
    // end of onRender, so relying on it alone leaves capture one frame out of phase.
    UI::RenderMetrics::get().setEnabled(m_ShowRenderMetrics);
    // Publish the previous frame's totals unconditionally, before any chart has a chance to
    // render this frame. Doing this here (not leaving it to record(), which only runs when a
    // chart actually renders) means a frame with zero chart activity still correctly publishes
    // an empty "last frame" instead of leaving the previous chart-bearing frame's totals in
    // place (see #875).
    UI::RenderMetrics::get().beginFrame(ImGui::GetFrameCount());
}

void ShellLayer::onRender()
{
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

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                         ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

    if (ImGui::Begin("##MainWindow", nullptr, windowFlags))
    {
        ImGui::PopStyleVar(3);

        renderTabBar();

        // Render content area with padding
        constexpr float CONTENT_PADDING_H = 12.0F;
        constexpr float CONTENT_PADDING_V = 4.0F;

        // Add padding by using a child window with border that provides internal padding
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CONTENT_PADDING_H, CONTENT_PADDING_V));

        // Let ImGui own child sizing so each panel can consume full available height without
        // shell-level scrollbar reservations that affect non-process tabs.
        if (ImGui::BeginChild("##ContentArea", ImVec2(0.0F, 0.0F), ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            m_Tabs.renderContent();
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
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
    // Add top edge padding for visual balance
    constexpr float TOP_EDGE_PADDING = 4.0F;
    ImGui::Dummy(ImVec2(0.0F, TOP_EDGE_PADDING));

    // Add left indent to align main tabs with panel tabs below (content area has 12px padding)
    constexpr float LEFT_INDENT = 12.0F;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + LEFT_INDENT);

    // Add horizontal padding inside tabs and vertical padding for taller tabs
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0F, 10.0F));

    if (ImGui::BeginTabBar("##MainTabBar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip))
    {
        // Track previous tab to emit change event if selection changes
        const auto* previousTab = &m_Tabs.activeTab();

        std::size_t index = 0;
        for (const auto& tab : m_Tabs.tabs())
        {
            if (ImGui::BeginTabItem(tab.label(), nullptr, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton))
            {
                m_Tabs.select(index);
                ImGui::EndTabItem();
            }
            ++index;
        }

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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, verticalPadding));

    if (ImGui::Begin("##StatusBar", nullptr, windowFlags))
    {
        // Show a persistent lock icon when running without elevated privileges
        if (m_HasReducedPrivileges)
        {
            ImGui::TextColored(theme.scheme().textWarning, ICON_FA_LOCK);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Limited data: running without elevated privileges");
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

        // Right-align FPS display
        const char* fpsText = "%.1f FPS (%.2f ms)";
        const float fpsWidth = ImGui::CalcTextSize(fpsText).x + 50.0F; // Extra space for numbers
        ImGui::SameLine(ImGui::GetWindowWidth() - fpsWidth);
        ImGui::Text("%.1f FPS (%.2f ms)",
                    static_cast<double>(m_FpsCounter.displayedFps()),
                    static_cast<double>(m_FpsCounter.frameTime() * 1000.0F));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace App
