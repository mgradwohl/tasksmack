#include "ShellLayer.h"

#include "Core/Application.h"

#include <imgui.h>
#include <implot.h>
#include <spdlog/spdlog.h>

namespace App
{

ShellLayer::ShellLayer() : Layer("ShellLayer")
{
}

void ShellLayer::onAttach()
{
    spdlog::info("ShellLayer attached");

    // Configure ImGui for docking
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    // Initialize panels
    m_ProcessesPanel.onAttach();
    m_SystemMetricsPanel.onAttach();

    spdlog::info("Panels initialized");
}

void ShellLayer::onDetach()
{
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

    // Sync selected PID from processes panel to details panel
    int32_t selectedPid = m_ProcessesPanel.selectedPid();
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
}

void ShellLayer::onRender()
{
    setupDockspace();
    renderMenuBar();

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

    // Optional demo windows
    if (m_ShowImGuiDemo)
    {
        ImGui::ShowDemoWindow(&m_ShowImGuiDemo);
    }
    if (m_ShowImPlotDemo)
    {
        ImPlot::ShowDemoWindow(&m_ShowImPlotDemo);
    }

    renderStatusBar();
}

void ShellLayer::setupDockspace()
{
    // Create a fullscreen dockspace
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Account for menu bar at top and status bar at bottom
    constexpr float menuBarHeight = 19.0F;
    constexpr float statusBarHeight = 22.0F;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + menuBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, viewport->WorkSize.y - menuBarHeight - statusBarHeight));
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
            ImGui::MenuItem("Details", nullptr, &m_ShowDetails);
            ImGui::Separator();
            if (ImGui::MenuItem("Dark Mode", nullptr, &m_DarkMode))
            {
                if (m_DarkMode)
                {
                    ImGui::StyleColorsDark();
                }
                else
                {
                    ImGui::StyleColorsLight();
                }
            }
            ImGui::Separator();
            ImGui::MenuItem("ImGui Demo", nullptr, &m_ShowImGuiDemo);
            ImGui::MenuItem("ImPlot Demo", nullptr, &m_ShowImPlotDemo);
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
                // TODO: Open about dialog
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }
}

void ShellLayer::renderStatusBar()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr float statusBarHeight = 22.0F;

    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + viewport->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x, statusBarHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, 2.0F));

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
}

} // namespace App
