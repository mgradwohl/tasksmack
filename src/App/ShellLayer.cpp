#include "ShellLayer.h"

#include "App/AboutLayer.h"
#include "App/SettingsLayer.h"
#include "Core/Application.h"
#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UserConfig.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>

namespace App
{

ShellLayer::ShellLayer() : Layer("ShellLayer")
{
}

void ShellLayer::onAttach()
{
    spdlog::info("ShellLayer attached");

    // Load user configuration
    auto& config = UserConfig::get();
    config.load();

    // Apply config to theme (must be after UILayer loads themes)
    config.applyToApplication();

    // Initialize panels
    m_ProcessesPanel.onAttach();
    m_SystemMetricsPanel.onAttach();

    // Share the process model with panels that render system-level aggregates
    if (auto* processModel = m_ProcessesPanel.processModel(); processModel != nullptr)
    {
        m_SystemMetricsPanel.setProcessModel(processModel);
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

void ShellLayer::onUpdate(float deltaTime)
{
    // Update FPS counter (average over ~0.5 seconds)
    m_FrameTime = deltaTime;
    m_FrameTimeAccumulator += deltaTime;
    m_FrameCount++;

    if (m_FrameTimeAccumulator >= 0.5F)
    {
        m_DisplayedFps = static_cast<float>(m_FrameCount) / m_FrameTimeAccumulator;
        m_FrameTimeAccumulator = 0.0F;
        m_FrameCount = 0;
    }

    // Update panels
    m_ProcessesPanel.onUpdate(deltaTime);
    m_SystemMetricsPanel.onUpdate(deltaTime);

    // Sync selected PID from processes panel to details panel
    const std::int32_t selectedPid = m_ProcessesPanel.selectedPid();
    m_ProcessDetailsPanel.setSelectedPid(selectedPid);

    // Find the selected process snapshot
    // Note: snapshots() returns a copy, so we store it to iterate safely
    const Domain::ProcessSnapshot* selectedSnapshot = nullptr;
    Domain::ProcessSnapshot cachedSnapshot;
    if (selectedPid != -1)
    {
        auto currentSnapshots = m_ProcessesPanel.snapshots();
        for (const auto& snap : currentSnapshots)
        {
            if (snap.pid == selectedPid)
            {
                cachedSnapshot = snap;
                selectedSnapshot = &cachedSnapshot;
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

    // Calculate status bar height
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    // Create fullscreen window that covers the viewport minus status bar
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
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
    const ImGuiStyle& style = ImGui::GetStyle();

    // Add top edge padding for visual balance
    constexpr float TOP_EDGE_PADDING = 4.0F;
    ImGui::Dummy(ImVec2(0.0F, TOP_EDGE_PADDING));

    // Tab bar with icons on the right
    const float availWidth = ImGui::GetContentRegionAvail().x;

    // Calculate icon button sizes (include 8px right edge padding)
    constexpr float RIGHT_EDGE_PADDING = 8.0F;
    const float iconButtonWidth = ImGui::GetFrameHeight();
    const float iconButtonSpacing = style.ItemSpacing.x;
    const float rightIconsWidth = (iconButtonWidth * 2.0F) + iconButtonSpacing + style.ItemSpacing.x + RIGHT_EDGE_PADDING;

    // Tab bar takes remaining width
    // Add horizontal padding inside tabs and vertical padding for taller tabs
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0F, 10.0F));

    if (ImGui::BeginTabBar("##MainTabBar", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton | ImGuiTabBarFlags_NoTooltip))
    {
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
    }

    ImGui::PopStyleVar();

    // Right-aligned icon buttons on the same row as tabs
    ImGui::SameLine(availWidth - rightIconsWidth + style.ItemSpacing.x);

    // Vertically center the icon buttons with the tabs
    // Tabs have 10px vertical padding, buttons have default (~4px), so offset by the difference
    constexpr float TAB_VERTICAL_PADDING = 10.0F;
    const float defaultVerticalPadding = style.FramePadding.y;
    const float verticalOffset = TAB_VERTICAL_PADDING - defaultVerticalPadding;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalOffset);

    // Help button (?)
    if (ImGui::Button(ICON_FA_CIRCLE_QUESTION))
    {
        if (auto* about = AboutLayer::instance(); about != nullptr)
        {
            about->requestOpen();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("About TaskSmack");
    }

    ImGui::SameLine();

    // Settings button (gear) - also needs vertical alignment like help button
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + verticalOffset);
    if (ImGui::Button(ICON_FA_GEAR))
    {
        if (auto* settings = SettingsLayer::instance(); settings != nullptr)
        {
            settings->requestOpen();
        }
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Settings");
    }
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
