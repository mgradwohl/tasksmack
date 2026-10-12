/// @file test_SystemMetricsPanelRender.cpp
/// @brief The System Metrics panel as a whole (#1546), run headless with mock probes through its
/// probe-factory seam: onAttach() builds its models from the mocks (no platform probe, no synthetic
/// scenario), the Overview draws its charts from their readings, the tabs switch to their sections, an
/// inactive panel draws nothing, and a detached one says its model is gone.

#include "App/Panels/SystemMetricsPanel.h"
#include "Core/ApplicationEvents.h"
#include "Domain/ProcessModel.h"
#include "Mocks/MockProbes.h"
#include "Platform/PowerTypes.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SystemTypes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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
        eth0.linkSpeedMbps = 1000;
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

/// A battery the power probe reports, discharging at 80 %.
[[nodiscard]] std::unique_ptr<TestMocks::MockPowerProbe> mockBattery()
{
    auto probe = std::make_unique<TestMocks::MockPowerProbe>();
    Platform::PowerCapabilities caps;
    caps.hasBattery = true;
    caps.hasChargePercent = true;
    caps.hasPowerRate = true;
    probe->setCapabilities(caps);
    Platform::PowerCounters counters;
    counters.state = Platform::BatteryState::Discharging;
    counters.chargePercent = 80;
    counters.powerNowW = 12.5;
    probe->setCounters(counters);
    return probe;
}

/// A process model over two processes whose threads, page faults, descriptors and energy it can read.
[[nodiscard]] std::shared_ptr<Domain::ProcessModel> mockProcessModel()
{
    auto probe = std::make_unique<TestMocks::MockProcessProbe>();
    Platform::ProcessCapabilities caps;
    caps.hasThreadCount = true;
    caps.hasHandleCount = true;
    caps.hasPageFaults = true;
    caps.hasPowerUsage = true;
    probe->setCapabilities(caps);
    Platform::ProcessCounters init = TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0);
    init.threadCount = 1;
    init.handleCount = 12;
    Platform::ProcessCounters server = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
    server.threadCount = 8;
    server.handleCount = 40;
    probe->withProcess(init).withProcess(server);
    return std::make_shared<Domain::ProcessModel>(std::move(probe));
}

// #1546: with samples arriving (the sampler's, and the process model's), onUpdate() adopting them as the
// app's update loop does, the Overview's charts draw their history: CPU, the battery chart with its
// details, and the threads/page faults/descriptors chart from the process model. Hovering a chart draws
// its tooltip.
TEST_F(SystemMetricsPanelRenderTest, OverviewChartsDrawTheirHistoryAsSamplesArrive)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        [] { return SystemMetricsProbes{.system = mockSystem(), .power = mockBattery(), .disk = nullptr, .gpu = nullptr}; });
    panel->onAttach();
    const std::shared_ptr<Domain::ProcessModel> processes = mockProcessModel();
    panel->setProcessModel(processes);
    panel->setSamplingInterval(std::chrono::milliseconds(100));

    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int frames = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        processes->refresh();
        panel->onUpdate(1.0F / 60.0F);
        text = renderAndCapture(*panel);
        ++frames;
        if (frames > 30 && text.contains("Battery") && text.contains("Threads, Page Faults"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(text.contains("CPU Usage")) << text;
    EXPECT_TRUE(text.contains("Power & Battery")) << text; // the process probe measures power too
    EXPECT_TRUE(text.contains("80")) << text;              // the battery's charge, in its Now bar
    EXPECT_TRUE(text.contains("Threads, Page Faults")) << text;
    EXPECT_GE(ImPlot::GetCurrentContext()->Plots.GetBufSize(), 4); // CPU, memory, power, resources

    // Hover across the charts: each plot's tooltip path runs (and must not assert).
    ImGuiIO& io = ImGui::GetIO();
    for (int step = 1; step < 20; ++step)
    {
        io.AddMousePosEvent(DISPLAY_WIDTH * 0.4F, DISPLAY_HEIGHT * static_cast<float>(step) / 20.0F);
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
    }
    panel->onDetach();
}

/// The x-axis span, in seconds, of the first plot drawn: the Overview's CPU chart, which comes first.
/// 0 when none was drawn.
[[nodiscard]] double cpuChartXSpan()
{
    ImPlotContext& context = *ImPlot::GetCurrentContext();
    const ImPlotPlot* plot = context.Plots.GetBufSize() > 0 ? context.Plots.GetByIndex(0) : nullptr;
    return plot != nullptr ? plot->Axes[ImAxis_X1].Range.Size() : 0.0;
}

// The Settings dialog's events (#1078, #1145): a new history length re-spans the Overview's charts to
// that window at once, an unchanged one changes nothing, and a new refresh rate is taken while the
// charts keep drawing.
TEST_F(SystemMetricsPanelRenderTest, TheChartsFollowTheHistoryAndRefreshSettings)
{
    const auto panel = attachedPanel();
    for (int frame = 0; frame < 3; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
    }
    const double before = cpuChartXSpan();
    ASSERT_GT(before, 0.0);

    Core::HistoryDurationChangedEvent shorter(60);
    panel->onEvent(shorter);
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const double after = cpuChartXSpan();
    EXPECT_LT(after, before);
    EXPECT_NEAR(after, 60.0, 6.0);

    Core::HistoryDurationChangedEvent same(60);
    panel->onEvent(same);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_DOUBLE_EQ(cpuChartXSpan(), after);

    Core::RefreshRateChangedEvent faster(250);
    panel->onEvent(faster);
    panel->onUpdate(1.0F / 60.0F);
    EXPECT_TRUE(renderAndCapture(*panel).contains("CPU Usage"));
    panel->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, PickingAnInterfaceShowsItsLinkAndState)
{
    const auto panel = attachedPanel(/*network=*/true);
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(selectTab("Network and I/O"));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("[Up]")); // "Total" picked: no single link to show

    // The interface combo sits in whichever child window draws the section: open it from there.
    const ImGuiWindow* combo = nullptr;
    for (int i = 0; i < GImGui->Windows.Size && combo == nullptr; ++i)
    {
        ImGui::ActivateItemByID(ImHashStr("##NetworkInterface", 0, GImGui->Windows[i]->ID));
        static_cast<void>(renderAndCapture(*panel));
        static_cast<void>(renderAndCapture(*panel));
        if (!GImGui->OpenPopupStack.empty() && std::string_view(GImGui->OpenPopupStack.back().Window->Name).contains("Combo"))
        {
            combo = GImGui->OpenPopupStack.back().Window;
        }
    }
    ASSERT_NE(combo, nullptr);
    ImGui::ActivateItemByID(ImHashStr("eth0", 0, combo->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Link: 1 Gbit/s")) << text; // rated in bits, as the adapter is
    EXPECT_TRUE(text.contains("[Up]")) << text;
    panel->onDetach();
}

} // namespace
} // namespace App
