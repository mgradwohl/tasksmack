#include "ShellLayer.h"

#include "App/AboutLayer.h"
#include "Core/Application.h"
#include "Core/Layer.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"
#include "UserConfig.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
// clang-format on
#else
#include <cerrno>
#include <cstring>

#include <sys/wait.h>
#include <unistd.h>
#endif

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

    for (const int stop : stops)
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

    for (const int stop : stops)
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

/// Open a file with the platform's default application
void openFileWithDefaultEditor(const std::filesystem::path& filePath)
{
    if (!std::filesystem::exists(filePath))
    {
        spdlog::error("Cannot open file: {} does not exist", filePath.string());
        return;
    }

#ifdef _WIN32
    // Windows: Prefer Notepad to avoid unexpected handlers (e.g., Node.js associations)
    const std::wstring wpath = filePath.wstring();
    auto* const result = ShellExecuteW(nullptr, L"open", L"notepad.exe", wpath.c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecuteW returns a value > 32 on success
    if (reinterpret_cast<intptr_t>(result) <= 32)
    {
        // Fallback to shell default association if Notepad failed to launch
        auto* const fallback = ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(fallback) <= 32)
        {
            spdlog::error("Failed to open file with Notepad or default handler: {}", filePath.string());
            return;
        }
    }
    spdlog::info("Opened config file: {}", filePath.string());
#else
    // Linux: Use double-fork to safely spawn xdg-open without creating zombies
    // First fork creates a child that will be reaped
    const pid_t pid = fork();
    if (pid == -1)
    {
        spdlog::error("Failed to fork process for xdg-open: {}", strerror(errno));
        return;
    }

    if (pid == 0)
    {
        // First child: fork again to create grandchild
        const pid_t grandchild = fork();
        if (grandchild == -1)
        {
            spdlog::error("Failed to fork grandchild: {}", strerror(errno));
            _exit(EXIT_FAILURE);
        }

        if (grandchild == 0)
        {
            // Grandchild: exec xdg-open (will be adopted by init when first child exits)
            const std::string pathStr = filePath.string();
            // Safe: no shell involved, arguments are separate
            execlp("xdg-open", "xdg-open", pathStr.c_str(), nullptr);
            // execlp only returns on error
            spdlog::error("Failed to exec xdg-open: {}", strerror(errno));
            _exit(EXIT_FAILURE);
        }
        // First child exits immediately (grandchild will be adopted by init)
        _exit(0);
    }

    // Parent: wait for first child to prevent zombie
    int status = 0;
    const pid_t waited = waitpid(pid, &status, 0);
    if (waited == -1)
    {
        spdlog::error("waitpid failed while waiting for xdg-open child process: {}", strerror(errno));
    }
    else
    {
        spdlog::info("Opened config file with xdg-open: {}", filePath.string());
    }
#endif
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

    // Restore ImGui layout state (window positions, docking layout, etc.)
    config.applyImGuiLayout();

    // Restore panel visibility from config
    const auto& settings = config.settings();
    m_ShowProcesses = settings.showProcesses;
    m_ShowMetrics = settings.showMetrics;
    m_ShowDetails = settings.showDetails;

    // Configure ImGui for docking
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

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

    // Capture ImGui layout state (window positions, docking layout, etc.)
    config.captureImGuiLayout();

    // Save panel visibility
    auto& settings = config.settings();
    settings.showProcesses = m_ShowProcesses;
    settings.showMetrics = m_ShowMetrics;
    settings.showDetails = m_ShowDetails;

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
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));

    ImGui::Begin("DockSpaceWindow", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    // Create the dockspace
    const ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");
    ImGui::DockSpace(dockspaceId, ImVec2(0.0F, 0.0F), ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::End();
}

void ShellLayer::renderMenuBar()
{
    // Increase vertical padding for menu items to center text better
    const float menuBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().FramePadding.y * 2.0F);
    const float verticalPadding = (menuBarHeight - ImGui::GetFontSize()) * 0.5F;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, verticalPadding));

    if (ImGui::BeginMainMenuBar())
    {
        if (ImGui::BeginMenu(ICON_FA_FILE " File"))
        {
            if (ImGui::MenuItem(ICON_FA_DOOR_OPEN " Exit", "Alt+F4"))
            {
                Core::Application::get().stop();
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_EYE " View"))
        {
            ImGui::MenuItem(ICON_FA_LIST " Processes", nullptr, &m_ShowProcesses);
            ImGui::MenuItem(ICON_FA_COMPUTER " System Metrics", nullptr, &m_ShowMetrics);
            ImGui::MenuItem(ICON_FA_CIRCLE_INFO " Details", nullptr, &m_ShowDetails);
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

                    // Ensure the change takes effect without waiting a full interval.
                    m_ProcessesPanel.requestRefresh();
                    m_SystemMetricsPanel.requestRefresh();
                }
            }

            ImGui::Separator();

            // Theme submenu (dynamically loaded from TOML files)
            if (ImGui::BeginMenu(ICON_FA_PALETTE " Theme"))
            {
                auto& theme = UI::Theme::get();
                const auto& themes = theme.discoveredThemes();
                auto currentIndex = theme.currentThemeIndex();

                for (std::size_t i = 0; i < themes.size(); ++i)
                {
                    const bool selected = (currentIndex == i);
                    if (ImGui::MenuItem(themes[i].name.c_str(), nullptr, selected))
                    {
                        theme.setTheme(i);
                    }
                }
                ImGui::EndMenu();
            }

            // Font size submenu
            if (ImGui::BeginMenu(ICON_FA_FONT " Font Size"))
            {
                auto& theme = UI::Theme::get();
                auto currentSize = theme.currentFontSize();

                for (const auto fontSize : UI::ALL_FONT_SIZES)
                {
                    const auto& cfg = theme.fontConfig(fontSize);
                    const bool selected = (currentSize == fontSize);
                    const std::string label(cfg.name);
                    if (ImGui::MenuItem(label.c_str(), nullptr, selected))
                    {
                        theme.setFontSize(fontSize);
                    }
                }

                ImGui::Separator();

                // Quick adjust shortcuts
                if (ImGui::MenuItem(ICON_FA_PLUS " Increase", "Ctrl++"))
                {
                    theme.increaseFontSize();
                }
                if (ImGui::MenuItem(ICON_FA_MINUS " Decrease", "Ctrl+-"))
                {
                    theme.decreaseFontSize();
                }

                ImGui::EndMenu();
            }

            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_WRENCH " Tools"))
        {
            if (ImGui::MenuItem(ICON_FA_FILE_PEN " Open Config File..."))
            {
                const auto& configPath = UserConfig::get().configPath();
                openFileWithDefaultEditor(configPath);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(ICON_FA_GEARS " Options..."))
            {
                // Options dialog: See GitHub issue for planned implementation
                // Feature: Global settings dialog for themes, refresh rates, column presets
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu(ICON_FA_CIRCLE_QUESTION " Help"))
        {
            if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO " About TaskSmack"))
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

void ShellLayer::renderStatusBar() const
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Calculate height dynamically based on font size for proper scaling
    const float statusBarHeight = ImGui::GetFrameHeight() + (ImGui::GetStyle().WindowPadding.y * 2.0F);

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;

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
