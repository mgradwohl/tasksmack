#include "ShellLayer.h"

#include "Core/Application.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "TitleBarLayer.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UserConfig.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace App
{

ShellLayer::ShellLayer() : Layer("ShellLayer")
{}

void ShellLayer::onAttach()
{
    spdlog::info("ShellLayer attached");

    // Load user configuration and apply to theme
    auto& config = UserConfig::get();
    config.load();
    config.applyToApplication();

    // Initialize panels
    m_ProcessesPanel.onAttach();
    m_SystemMetricsPanel.onAttach();

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

    const auto [x, y] = window.getPosition();
    settings.windowPosX = x;
    settings.windowPosY = y;

    settings.windowMaximized = window.isMaximized();

    config.save();

    m_SystemMetricsPanel.onDetach();
    m_ProcessesPanel.onDetach();
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
    m_ProcessesPanel.onEvent(event);
    m_ProcessDetailsPanel.onEvent(event);
    m_SystemMetricsPanel.onEvent(event);
}

void ShellLayer::onUpdate(float deltaTime)
{
    // Update FPS counter (average over ~0.5 seconds)
    m_FrameTime = deltaTime;
    m_FrameTimeAccumulator += deltaTime;
    m_FrameCount++;

    if (m_FrameTimeAccumulator >= FPS_AVERAGE_WINDOW_SECONDS)
    {
        m_DisplayedFps = static_cast<float>(m_FrameCount) / m_FrameTimeAccumulator;
        m_FrameTimeAccumulator = 0.0F;
        m_FrameCount = 0;
    }

    // Update panels
    m_ProcessesPanel.onUpdate(deltaTime);
    m_SystemMetricsPanel.onUpdate(deltaTime);

    // Find the selected process snapshot for rendering
    // Note: Selection is now coordinated via ProcessSelectedEvent, but we still need
    // to look up the snapshot for ProcessDetailsPanel to render
    const Domain::ProcessSnapshot* selectedSnapshot = nullptr;
    Domain::ProcessSnapshot cachedSnapshot;
    const std::int32_t selectedPid = m_ProcessesPanel.selectedPid();
    if (selectedPid != -1)
    {
        auto currentSnapshots = m_ProcessesPanel.snapshots();
        for (const auto& snap : currentSnapshots)
        {
            if (snap.pid == selectedPid)
            {
                cachedSnapshot = snap;
                selectedSnapshot = &cachedSnapshot;

                // Debug: Log when GPU data becomes available for the selected PID.
                // This avoids spamming logs every frame while a GPU-using process is selected.
                const bool hasGpuData = (!snap.gpuDevices.empty() || (snap.gpuMemoryBytes > 0));

                if ((selectedPid != m_LastGpuLogPid) || !m_LastGpuLogHasData)
                {
                    if (hasGpuData)
                    {
                        spdlog::debug("ShellLayer: Selected PID {} has GPU data: devices='{}', mem={}",
                                      selectedPid,
                                      snap.gpuDevices,
                                      snap.gpuMemoryBytes);
                    }

                    m_LastGpuLogPid = selectedPid;
                    m_LastGpuLogHasData = hasGpuData;
                }
                break;
            }
        }
    }
    m_ProcessDetailsPanel.updateWithSnapshot(selectedSnapshot, deltaTime);

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
}

void ShellLayer::onRender()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Get dynamic title bar height (matches tab bars)
    const float titleBarHeight = TitleBarLayer::height();

    // Calculate status bar height
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    // Create fullscreen window that covers the viewport minus title bar and status bar
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + titleBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight - titleBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
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
        const ImVec2 contentSize(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);

        if (ImGui::BeginChild("##ContentArea", contentSize, ImGuiChildFlags_AlwaysUseWindowPadding))
        {
            switch (m_ActiveTab)
            {
            case ActiveTab::SystemOverview:
                m_SystemMetricsPanel.renderContent();
                break;
            case ActiveTab::Processes:
                m_ProcessesPanel.renderContent();
                break;
            case ActiveTab::ProcessDetails:
                m_ProcessDetailsPanel.renderContent();
                break;
            }
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
        const ActiveTab previousTab = m_ActiveTab;

        // Tab 1: System Overview (hostname)
        const std::string& hostname = m_SystemMetricsPanel.hostname();
        const std::string systemLabel = std::string(ICON_FA_COMPUTER) + "  " + hostname;
        if (ImGui::BeginTabItem(systemLabel.c_str(), nullptr, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton))
        {
            m_ActiveTab = ActiveTab::SystemOverview;
            ImGui::EndTabItem();
        }

        // Tab 2: Processes
        if (ImGui::BeginTabItem(ICON_FA_LIST "  Processes", nullptr, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton))
        {
            m_ActiveTab = ActiveTab::Processes;
            ImGui::EndTabItem();
        }

        // Tab 3: Process Details (shows process name or "Select a process")
        const std::string detailsLabel = std::string(ICON_FA_CIRCLE_INFO) + "  " + m_ProcessDetailsPanel.tabLabel();
        if (ImGui::BeginTabItem(detailsLabel.c_str(), nullptr, ImGuiTabItemFlags_NoCloseWithMiddleMouseButton))
        {
            m_ActiveTab = ActiveTab::ProcessDetails;
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();

        // Emit ActiveTabChangedEvent when tab selection changes
        if (previousTab != m_ActiveTab)
        {
            std::string tabName;
            switch (m_ActiveTab)
            {
            case ActiveTab::SystemOverview:
                tabName = "SystemOverview";
                break;
            case ActiveTab::Processes:
                tabName = "Processes";
                break;
            case ActiveTab::ProcessDetails:
                tabName = "ProcessDetails";
                break;
            }
            Core::ActiveTabChangedEvent evt(std::move(tabName));
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

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

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
        ImGui::Text("Ready");

        // Right-align FPS display
        const char* fpsText = "%.1f FPS (%.2f ms)";
        const float fpsWidth = ImGui::CalcTextSize(fpsText).x + 50.0F; // Extra space for numbers
        ImGui::SameLine(ImGui::GetWindowWidth() - fpsWidth);
        ImGui::Text("%.1f FPS (%.2f ms)", static_cast<double>(m_DisplayedFps), static_cast<double>(m_FrameTime * 1000.0F));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace App
