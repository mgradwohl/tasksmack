#include "ShellLayer.h"

#include "App/AboutLayer.h"
#include "Core/Application.h"
#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "UI/Theme.h"
#include "UserConfig.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

namespace App
{

namespace
{

[[nodiscard]] int snapRefreshIntervalMs(int value)
{
    // Sticky stops for common refresh rates.
    // Snap only when close enough to a stop (threshold scales with stop size).
    constexpr std::array<int, 4> stops = {100, 250, 500, 1000};

    int best = value;
    int bestDist = std::numeric_limits<int>::max();

    for (int stop : stops)
    {
        const int dist = std::abs(value - stop);
        // Make it "at least twice as sticky":
        // Previous: min(50, stop/5). Now: 2x that, capped at 100ms.
        const int baseThreshold = std::min(50, stop / 5);
        const int threshold = std::min(100, baseThreshold * 2);
        if (dist <= threshold && dist < bestDist)
        {
            best = stop;
            bestDist = dist;
        }
    }

    return best;
}

void drawRefreshPresetTicks(const ImVec2 frameMin, const ImVec2 frameMax, int minValue, int maxValue)
{
    if (maxValue <= minValue)
    {
        return;
    }

    constexpr std::array<int, 4> stops = {100, 250, 500, 1000};
    auto* drawList = ImGui::GetWindowDrawList();

    // Use a visible but subdued color; border can be too subtle on some themes.
    const ImU32 tickColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float w = frameMax.x - frameMin.x;

    // Keep ticks inside the slider frame.
    ImGui::PushClipRect(frameMin, frameMax, true);

    for (int stop : stops)
    {
        if (stop < minValue || stop > maxValue)
        {
            continue;
        }
        const float t = static_cast<float>(stop - minValue) / static_cast<float>(maxValue - minValue);
        const float x = frameMin.x + (t * w);

        // Slight inset looks nicer than touching the border.
        drawList->AddLine(ImVec2(x, frameMin.y + 2.0F), ImVec2(x, frameMax.y - 2.0F), tickColor, 1.0F);
    }

    ImGui::PopClipRect();
}

} // namespace

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

    // Restore panel visibility from config
    const auto& settings = config.settings();
    m_ShowProcesses = settings.showProcesses;
    m_ShowMetrics = settings.showMetrics;
    m_ShowDetails = settings.showDetails;
    m_ShowStorage = settings.showStorage;

    // Configure ImGui for docking
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Initialize panels
    m_ProcessesPanel.onAttach();
    m_SystemMetricsPanel.onAttach();
    m_StoragePanel.onAttach();

    spdlog::info("Panels initialized");
}

void ShellLayer::onDetach()
{
    // Save user configuration
    auto& config = UserConfig::get();
    config.captureFromApplication();

    // Save panel visibility
    auto& settings = config.settings();
    settings.showProcesses = m_ShowProcesses;
    settings.showMetrics = m_ShowMetrics;
    settings.showDetails = m_ShowDetails;
    settings.showStorage = m_ShowStorage;

    // Capture current window geometry/state.
    auto& window = Core::Application::get().getWindow();
    const auto [width, height] = window.getSize();
    settings.windowWidth = width;
    settings.windowHeight = height;

    const auto [x, y] = window.getPosition();
    settings.windowPosX = x;
    settings.windowPosY = y;

    settings.windowMaximized = window.isMaximized();

    config.save();

    m_StoragePanel.onDetach();
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
    m_StoragePanel.onUpdate(deltaTime);

    // Sync selected PID from processes panel to details panel
    std::int32_t selectedPid = m_ProcessesPanel.selectedPid();
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
    ImGuiIO& io = ImGui::GetIO();
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
    renderMenuBar();
    setupDockspace();

    // Render panels
    if (m_ShowProcesses)
    {
        m_ProcessesPanel.render(&m_ShowProcesses);
    }
    if (m_ShowMetrics)
    {
        m_SystemMetricsPanel.render(&m_ShowMetrics);
    }
    if (m_ShowStorage)
    {
        m_StoragePanel.render(&m_ShowStorage);
    }
    if (m_ShowDetails)
    {
        m_ProcessDetailsPanel.render(&m_ShowDetails);
    }
    renderStatusBar();
}

void ShellLayer::setupDockspace()
{
    // Create a fullscreen dockspace
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Account for status bar at bottom. The main menu bar already adjusts the viewport work area.
    // Calculate height dynamically based on font size for proper scaling
    float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

    ImGui::Begin("DockSpaceWindow", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    // Create the dockspace
    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

void ShellLayer::renderMenuBar()
{
    // Increase vertical padding for menu items to center text better
    float menuBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().FramePadding.y * 2.0F);
    float verticalPadding = (menuBarHeight - ImGui::GetFontSize()) * 0.5F;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, verticalPadding));

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                Core::Application::get().stop();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Processes", nullptr, &m_ShowProcesses);
            ImGui::MenuItem("System Metrics", nullptr, &m_ShowMetrics);
            ImGui::MenuItem("Storage", nullptr, &m_ShowStorage);
            ImGui::MenuItem("Details", nullptr, &m_ShowDetails);
            ImGui::Separator();

            // Sampling / refresh interval (shared)
            {
                auto& settings = UserConfig::get().settings();
                const int beforeMs = settings.refreshIntervalMs;
                int refreshIntervalMs = beforeMs;

                ImGui::SetNextItemWidth(220.0F);
                const bool sliderChanged = ImGui::SliderInt("Refresh (ms)",
                                                            &refreshIntervalMs,
                                                            Domain::Sampling::REFRESH_INTERVAL_MIN_MS,
                                                            Domain::Sampling::REFRESH_INTERVAL_MAX_MS);

                // Draw tick marks for preset values (100/250/500/1000ms) on the actual slider frame.
                drawRefreshPresetTicks(ImGui::GetItemRectMin(),
                                       ImGui::GetItemRectMax(),
                                       Domain::Sampling::REFRESH_INTERVAL_MIN_MS,
                                       Domain::Sampling::REFRESH_INTERVAL_MAX_MS);

                // Snap when the user finishes editing (mouse release / enter).
                const bool releasedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
                if (releasedAfterEdit)
                {
                    refreshIntervalMs = snapRefreshIntervalMs(refreshIntervalMs);
                }

                const bool snappedChanged = (refreshIntervalMs != beforeMs);
                if (sliderChanged || snappedChanged)
                {
                    settings.refreshIntervalMs = refreshIntervalMs;

                    const auto interval = std::chrono::milliseconds(settings.refreshIntervalMs);
                    m_ProcessesPanel.setSamplingInterval(interval);
                    m_SystemMetricsPanel.setSamplingInterval(interval);
                    m_StoragePanel.setSamplingInterval(interval);

                    // Ensure the change takes effect without waiting a full interval.
                    m_ProcessesPanel.requestRefresh();
                    m_SystemMetricsPanel.requestRefresh();
                    m_StoragePanel.requestRefresh();
                }
            }

            ImGui::Separator();

            // Theme submenu (dynamically loaded from TOML files)
            if (ImGui::BeginMenu("Theme"))
            {
                auto& theme = UI::Theme::get();
                const auto& themes = theme.discoveredThemes();
                auto currentIndex = theme.currentThemeIndex();

                for (std::size_t i = 0; i < themes.size(); ++i)
                {
                    bool selected = (currentIndex == i);
                    if (ImGui::MenuItem(themes[i].name.c_str(), nullptr, selected))
                    {
                        theme.setTheme(i);
                    }
                }
                ImGui::EndMenu();
            }

            // Font size submenu
            if (ImGui::BeginMenu("Font Size"))
            {
                auto& theme = UI::Theme::get();
                auto currentSize = theme.currentFontSize();

                for (const auto fontSize : UI::ALL_FONT_SIZES)
                {
                    const auto& cfg = theme.fontConfig(fontSize);
                    bool selected = (currentSize == fontSize);
                    const std::string label(cfg.name);
                    if (ImGui::MenuItem(label.c_str(), nullptr, selected))
                    {
                        theme.setFontSize(fontSize);
                    }
                }

                ImGui::Separator();

                // Quick adjust shortcuts
                if (ImGui::MenuItem("Increase", "Ctrl++"))
                {
                    theme.increaseFontSize();
                }
                if (ImGui::MenuItem("Decrease", "Ctrl+-"))
                {
                    theme.decreaseFontSize();
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            if (ImGui::MenuItem("Options..."))
            {
                // TODO: Open options dialog
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About TaskSmack"))
            {
                if (auto* about = AboutLayer::instance(); about != nullptr)
                {
                    about->requestOpen();
                }
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
    ImGui::PopStyleVar();
}

void ShellLayer::renderStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Calculate height dynamically based on font size for proper scaling
    float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking;

    // Use theme colors for status bar
    const auto& theme = UI::Theme::get();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.scheme().statusBarBg);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.scheme().border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F); // Show top border
    // Center text vertically within the status bar
    float verticalPadding = (statusBarHeight - ImGui::GetFontSize()) * 0.5F;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, verticalPadding));

    if (ImGui::Begin("##StatusBar", nullptr, windowFlags))
    {
        ImGui::Text("Ready");

        // Right-align FPS display
        const char* fpsText = "%.1f FPS (%.2f ms)";
        float fpsWidth = ImGui::CalcTextSize(fpsText).x + 50.0F; // Extra space for numbers
        ImGui::SameLine(ImGui::GetWindowWidth() - fpsWidth);
        ImGui::Text("%.1f FPS (%.2f ms)", static_cast<double>(m_DisplayedFps), static_cast<double>(m_FrameTime * 1000.0F));
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace App
