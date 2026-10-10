/// @file test_SystemMetricsPanelRender.cpp
/// @brief The System Metrics panel as a whole (#1546), run headless with mock probes through its
/// probe-factory seam: onAttach() builds its models from the mocks (no platform probe, no synthetic
/// scenario), the Overview draws its charts from their readings, the tabs switch to their sections, an
/// inactive panel draws nothing, and a detached one says its model is gone.

#include "App/Panels/SystemMetricsPanel.h"
#include "Core/ApplicationEvents.h"
#include "Mocks/MockProbes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;

using SeriesLabels = std::set<std::string>;

/// A mock system probe reading a 4-core machine at 25% CPU and 50% memory; with @p network, network
/// counters and one interface too.
[[nodiscard]] std::unique_ptr<TestMocks::MockSystemProbe> mockSystem(bool network = false)
{
    auto probe = std::make_unique<TestMocks::MockSystemProbe>();
    Platform::SystemCounters counters = TestMocks::makeSystemCounters(TestMocks::makeCpuAtUsage(25.0),
                                                                      TestMocks::makeMemoryAtUsage(50.0),
                                                                      3600,
                                                                      {TestMocks::makeCpuAtUsage(10.0),
                                                                       TestMocks::makeCpuAtUsage(20.0),
                                                                       TestMocks::makeCpuAtUsage(30.0),
                                                                       TestMocks::makeCpuAtUsage(40.0)});
    counters.hostname = "testhost";
    if (network)
    {
        counters.netRxBytes = 1'000'000;
        counters.netTxBytes = 500'000;
        Platform::SystemCounters::InterfaceCounters eth0;
        eth0.name = "eth0";
        eth0.displayName = "eth0";
        eth0.rxBytes = 1'000'000;
        eth0.txBytes = 500'000;
        eth0.isUp = true;
        counters.networkInterfaces.push_back(eth0);
    }
    probe->setCounters(std::move(counters));
    Platform::SystemCapabilities caps;
    caps.hasPerCoreCpu = true;
    caps.hasMemoryAvailable = true;
    caps.hasUptime = true;
    caps.hasNetworkCounters = network;
    probe->setCapabilities(caps);
    return probe;
}

class SystemMetricsPanelRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        m_ImPlot = ImPlot::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// A panel whose models read the mocks: a system probe, and no disk, battery or GPU.
    [[nodiscard]] static std::unique_ptr<SystemMetricsPanel> attachedPanel(bool network = false)
    {
        auto panel = std::make_unique<SystemMetricsPanel>(
            [network] { return SystemMetricsProbes{.system = mockSystem(network), .power = nullptr, .disk = nullptr, .gpu = nullptr}; });
        panel->onAttach();
        return panel;
    }

    /// One frame of a window filling the display, drawing @p panel's content, and the text it drew.
    [[nodiscard]] static std::string renderAndCapture(SystemMetricsPanel& panel)
    {
        std::string captured;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        panel.renderContent();
        captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        return captured;
    }

    /// Asks the panel's tab bar to select the tab whose label contains @p name on the next frame, as a
    /// click on it would. False when there is no such tab.
    [[nodiscard]] static bool selectTab(std::string_view name)
    {
        ImGuiContext& g = *GImGui;
        for (int i = 0; i < g.TabBars.GetMapSize(); ++i)
        {
            ImGuiTabBar* bar = g.TabBars.TryGetMapData(i);
            if (bar == nullptr)
            {
                continue;
            }
            for (ImGuiTabItem& tab : bar->Tabs)
            {
                if (std::string_view(ImGui::TabBarGetTabName(bar, &tab)).contains(name))
                {
                    ImGui::TabBarQueueFocus(bar, &tab);
                    return true;
                }
            }
        }
        return false;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(SystemMetricsPanelRenderTest, OverviewDrawsTheMockMachinesCharts)
{
    const auto panel = attachedPanel();
    EXPECT_EQ(panel->hostname(), "testhost");
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);

    EXPECT_TRUE(text.contains("Overview")) << text;
    EXPECT_TRUE(text.contains("CPU Usage")) << text;
    EXPECT_TRUE(text.contains("Memory & Swap")) << text;
    EXPECT_TRUE(text.contains("CPU Cores")) << text;               // a tab: the mock has per-core counters
    EXPECT_GE(ImPlot::GetCurrentContext()->Plots.GetBufSize(), 2); // CPU and memory at least
    panel->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, TabsSwitchToTheirSections)
{
    const auto panel = attachedPanel(/*network=*/true);
    static_cast<void>(renderAndCapture(*panel));

    // No GPU probe: the GPU tab says monitoring is unavailable rather than drawing charts.
    ASSERT_TRUE(selectTab("GPU"));
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("GPU monitoring is not available")) << text;

    // The Network and I/O tab: the mock's interface in its status table, and, with no disk probe, the
    // storage model's empty Disk I/O chart below it.
    ASSERT_TRUE(selectTab("Network and I/O"));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Network Throughput")) << text;
    EXPECT_TRUE(text.contains("Interface Status")) << text;
    EXPECT_TRUE(text.contains("eth0")) << text;
    EXPECT_TRUE(text.contains("Disk I/O History")) << text;

    // CPU Cores: the mock's four cores counted. Per-core usage needs a second sample (a delta), and the
    // panel has taken one, so the cells say so rather than drawing a guess.
    ASSERT_TRUE(selectTab("CPU Cores"));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("(4 logical processors)")) << text;
    EXPECT_TRUE(text.contains("No per-core data")) << text;
    panel->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, TheNetworkTabNeedsNetworkCounters)
{
    const auto panel = attachedPanel(/*network=*/false);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(selectTab("Network and I/O")); // not drawn at all
    EXPECT_TRUE(selectTab("Overview"));
    panel->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, AnInactivePanelDrawsNothingAndADetachedOneSaysSo)
{
    const auto panel = attachedPanel();
    Core::ActiveTabChangedEvent away("Processes");
    panel->onEvent(away);
    EXPECT_TRUE(renderAndCapture(*panel).empty());

    Core::ActiveTabChangedEvent back("SystemOverview");
    panel->onEvent(back);
    EXPECT_TRUE(renderAndCapture(*panel).contains("CPU Usage"));

    panel->onDetach();
    EXPECT_TRUE(renderAndCapture(*panel).contains("System data unavailable"));
}

} // namespace
} // namespace App
