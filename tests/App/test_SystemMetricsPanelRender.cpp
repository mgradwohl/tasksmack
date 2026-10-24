/// @file test_SystemMetricsPanelRender.cpp
/// @brief The System Metrics panel as a whole (#1546), run headless with mock probes through its
/// probe-factory seam: its sampler thread builds its models' probes from the mocks (no platform probe,
/// no synthetic scenario; #2731: onAttach() itself calls no factory), the Overview draws its charts from
/// their readings, the tabs switch to their sections (and a requested sub-tab is brought forward once it
/// is drawn, #2869), an inactive panel draws nothing, and a detached one says its model is gone. Its
/// sampler runs at the refresh setting in every UI state (#2731).

#include "App/Panels/SystemMetricsPanel.h"
#include "App/SelectOverride.h"
#include "Core/ApplicationEvents.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockDiskProbe.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/PowerTypes.h"
#include "Platform/ProcessTypes.h"
#include "Platform/StorageTypes.h"
#include "Platform/SystemTypes.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <latch>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;

using SeriesLabels = std::set<std::string>;

/// onAttach(), then the seed the sampler thread takes (#2731), adopted as the first frame after it
/// would: the tests below read the models and the hostname as they stand after it.
void attachAndSeed(SystemMetricsPanel& panel)
{
    panel.onAttach();
    ASSERT_TRUE(panel.waitForFirstSample()) << "the sampler thread did not build the probes and seed the models in time";
}

/// A mock system probe reading a 4-core machine at 25% CPU and 50% memory; with @p network, network
/// counters and one interface too: @p interfaceName, linked at @p linkSpeedMbps (0: unknown).
[[nodiscard]] std::unique_ptr<TestMocks::MockSystemProbe>
mockSystem(bool network = false, const char* interfaceName = "eth0", std::uint64_t linkSpeedMbps = 1000)
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
        eth0.name = interfaceName;
        eth0.displayName = interfaceName;
        eth0.rxBytes = 1'000'000;
        eth0.txBytes = 500'000;
        eth0.isUp = true;
        eth0.linkSpeedMbps = linkSpeedMbps;
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
        attachAndSeed(*panel);
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

// #2869: requestTab() (TASKSMACK_MACHINE_TAB, TASKSMACK_DEMO's machine=) brings a sub-tab forward with
// ImGuiTabItemFlags_SetSelected, which ImGui applies on the next frame, and the request is done once
// the sub-tab reports selected.
TEST_F(SystemMetricsPanelRenderTest, ARequestedTabIsSelected)
{
    const auto panel = attachedPanel(/*network=*/true);
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("CPU Usage")) << text; // the Overview, the default
    EXPECT_FALSE(text.contains("Interface Status")) << text;

    panel->requestTab(SelectOverride::MachineTab::Network);
    EXPECT_TRUE(panel->requestedTabPending());
    static_cast<void>(renderAndCapture(*panel)); // submitted with the flag
    EXPECT_TRUE(panel->requestedTabPending()) << "ImGui applies SetSelected a frame late";
    text = renderAndCapture(*panel);
    EXPECT_FALSE(panel->requestedTabPending()) << "done once the sub-tab reported selected";
    EXPECT_TRUE(text.contains("Network Throughput")) << text;
    EXPECT_TRUE(text.contains("Interface Status")) << text;
    EXPECT_FALSE(text.contains("CPU Usage")) << text;

    // Back to the Overview, then the CPU Cores tab, each the same way.
    panel->requestTab(SelectOverride::MachineTab::Overview);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("CPU Usage")) << text;
    EXPECT_FALSE(panel->requestedTabPending());
    panel->requestTab(SelectOverride::MachineTab::Cores);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("(4 logical processors)")) << text;
    EXPECT_FALSE(panel->requestedTabPending());
    panel->onDetach();
}

// #2869: a startup request (ShellLayer::onAttach() makes it before the first frame) outlives the frames
// before the first sample, on which CPU Cores is not drawn at all, and takes effect once the seed brings
// the sub-tab; Process Details' one-shot request would have been spent on the first frame.
TEST_F(SystemMetricsPanelRenderTest, ARequestedTabWaitsForTheSampleThatDrawsIt)
{
    const auto gate = std::make_shared<std::latch>(1); // holds the factory, so no publication can arrive yet
    const auto panel = std::make_unique<SystemMetricsPanel>(
        [gate]
        {
            gate->wait();
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr};
        });
    panel->onAttach();
    panel->requestTab(SelectOverride::MachineTab::Cores);
    panel->onUpdate(1.0F / 60.0F);
    for (int frame = 0; frame < 3; ++frame)
    {
        const std::string before = renderAndCapture(*panel);
        EXPECT_FALSE(before.contains("CPU Cores")) << "no per-core readings yet: " << before;
        EXPECT_TRUE(panel->requestedTabPending()) << "kept while the sub-tab is not drawn, frame " << frame;
    }

    gate->count_down();
    ASSERT_TRUE(panel->waitForFirstSample());
    static_cast<void>(renderAndCapture(*panel));
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("(4 logical processors)")) << "the seed brought the CPU Cores tab, and the request took it: " << after;
    EXPECT_FALSE(after.contains("CPU Usage")) << after;
    EXPECT_FALSE(panel->requestedTabPending());
    panel->onDetach();
}

// #2869: a request for a sub-tab this machine never draws (no NPU) is dropped, with nothing selected,
// after MAX_MACHINE_TAB_FRAMES frames drawn with a snapshot, so no stale request is left.
TEST_F(SystemMetricsPanelRenderTest, ARequestedTabThisMachineLacksIsDroppedAfterTheFrameLimit)
{
    const auto panel = attachedPanel(/*network=*/false);
    panel->requestTab(SelectOverride::MachineTab::Npu);
    for (int frame = 0; frame < SelectOverride::MAX_MACHINE_TAB_FRAMES - 1; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        ASSERT_TRUE(panel->requestedTabPending()) << frame;
    }
    const std::string text = renderAndCapture(*panel);
    EXPECT_FALSE(panel->requestedTabPending());
    EXPECT_FALSE(text.contains("NPU")) << text;
    EXPECT_TRUE(text.contains("CPU Usage")) << "the Overview stays: " << text;
    panel->onDetach();
}

/// A panel whose GPU model reads a mock GPU probe with an iGPU and, with @p npu, an Intel AI Boost NPU.
[[nodiscard]] std::unique_ptr<SystemMetricsPanel> attachedPanelWithGpu(bool npu)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        [npu]
        {
            auto gpu = std::make_unique<TestMocks::MockGPUProbe>();
            gpu->withGPU("iGPU", "Intel(R) Arc(TM) 140T GPU", "Intel", true);
            if (npu)
            {
                gpu->withAccelerator("NPU", "Intel(R) AI Boost", "GPU_0x00000000_0x00018103");
            }
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = std::move(gpu)};
        });
    attachAndSeed(*panel);
    return panel;
}

// #1799: the NPU tab sits beside the GPU tab only when the GPU model lists an NPU or compute
// accelerator, and draws it there; the GPU tab keeps the GPU alone.
TEST_F(SystemMetricsPanelRenderTest, TheNpuTabAppearsOnlyWithAnAccelerator)
{
    const auto withoutNpu = attachedPanelWithGpu(/*npu=*/false);
    static_cast<void>(renderAndCapture(*withoutNpu));
    static_cast<void>(renderAndCapture(*withoutNpu));
    EXPECT_TRUE(selectTab("GPU"));
    EXPECT_FALSE(selectTab("NPU")); // not drawn at all
    withoutNpu->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, TheNpuTabDrawsTheAccelerator)
{
    const auto panel = attachedPanelWithGpu(/*npu=*/true);
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(selectTab("NPU"));
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("NPU Monitoring (1 device)")) << text;
    EXPECT_TRUE(text.contains("Intel(R) AI Boost [NPU]")) << text;
    EXPECT_FALSE(text.contains("Arc(TM) 140T")) << text;

    ASSERT_TRUE(selectTab("GPU"));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("GPU Monitoring (1 GPU)")) << text;
    EXPECT_FALSE(text.contains("AI Boost")) << text;
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

/// A process probe's capabilities with threads, page faults, descriptors and energy.
[[nodiscard]] Platform::ProcessCapabilities countingCapabilities()
{
    Platform::ProcessCapabilities caps;
    caps.hasThreadCount = true;
    caps.hasHandleCount = true;
    caps.hasPageFaults = true;
    caps.hasPowerUsage = true;
    return caps;
}

/// A process model over two processes whose threads, page faults, descriptors and energy it can read
/// (or what @p caps says it can); the second holds @p serverHandles handles (descriptors on Linux).
[[nodiscard]] std::shared_ptr<Domain::ProcessModel> mockProcessModel(std::int32_t serverHandles = 40,
                                                                     const Platform::ProcessCapabilities& caps = countingCapabilities())
{
    auto probe = std::make_unique<TestMocks::MockProcessProbe>();
    probe->setCapabilities(caps);
    Platform::ProcessCounters init = TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0);
    init.threadCount = 1;
    init.handleCount = 12;
    Platform::ProcessCounters server = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
    server.threadCount = 8;
    server.handleCount = serverHandles;
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
    attachAndSeed(*panel);
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

// #1891: resting the pointer on each Overview chart shows its tooltip -- CPU, Memory, Power & Battery and
// Threads/Page Faults/Handles each list their series at the hovered sample. Each plot is found by its
// rectangle and the pointer held there for two frames (a plot is hovered from the frame after the pointer
// arrives), rather than swept down one line of the window, which never rested on a plot.
TEST_F(SystemMetricsPanelRenderTest, EachOverviewChartShowsItsTooltipWhenHovered)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        [] { return SystemMetricsProbes{.system = mockSystem(), .power = mockBattery(), .disk = nullptr, .gpu = nullptr}; });
    attachAndSeed(*panel);
    const std::shared_ptr<Domain::ProcessModel> processes = mockProcessModel();
    panel->setProcessModel(processes);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;

    // History to hover: several samples of every chart.
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int frames = 0; std::chrono::steady_clock::now() < deadline; ++frames)
    {
        processes->refresh();
        panel->onUpdate(1.0F / 60.0F);
        text = renderAndCapture(*panel);
        if (frames > 40 && text.contains("Battery") && text.contains("Threads, Page Faults"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ImGuiIO& io = ImGui::GetIO();
    ImPlotContext& plots = *ImPlot::GetCurrentContext();
    int tooltips = 0;
    std::vector<ImRect> rects;
    for (int i = 0; i < plots.Plots.GetBufSize(); ++i)
    {
        if (const ImPlotPlot* plot = plots.Plots.GetByIndex(i);
            plot != nullptr && plot->PlotRect.GetWidth() > 4.0F && plot->PlotRect.Max.y < DISPLAY_HEIGHT && plot->PlotRect.Min.y >= 0.0F)
        {
            rects.push_back(plot->PlotRect);
        }
    }
    ASSERT_GE(rects.size(), 4U); // CPU, memory, power, resources
    for (const ImRect& rect : rects)
    {
        // Near the right edge: the newest samples.
        io.AddMousePosEvent(rect.Max.x - 3.0F, (rect.Min.y + rect.Max.y) * 0.5F);
        for (int frame = 0; frame < 3; ++frame)
        {
            panel->onUpdate(1.0F / 60.0F);
            static_cast<void>(renderAndCapture(*panel));
        }
        if (const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00"); tooltip != nullptr && tooltip->Active)
        {
            ++tooltips;
        }
    }
    EXPECT_GE(tooltips, 4) << "of " << rects.size() << " charts";
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

// #1891: where the platform reports I/O wait (Linux), the CPU chart's tooltip lists it as its own row.
TEST_F(SystemMetricsPanelRenderTest, TheCpuTooltipListsIoWaitWhereItIsReported)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        []
        {
            auto system = mockSystem();
            Platform::SystemCapabilities caps = system->capabilities();
            caps.hasIoWait = true;
            system->setCapabilities(caps);
            return SystemMetricsProbes{.system = std::move(system), .power = nullptr, .disk = nullptr, .gpu = nullptr};
        });
    attachAndSeed(*panel);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int frames = 0; frames < 40 && std::chrono::steady_clock::now() < deadline; ++frames)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // The CPU chart is the Overview's first plot.
    const ImPlotPlot* cpu = ImPlot::GetCurrentContext()->Plots.GetByIndex(0);
    ASSERT_NE(cpu, nullptr);
    const ImRect rect = cpu->PlotRect;
    ImGui::GetIO().AddMousePosEvent(rect.Max.x - 3.0F, (rect.Min.y + rect.Max.y) * 0.5F);
    bool shown = false;
    for (int frame = 0; frame < 3; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
        shown = tooltip != nullptr && tooltip->Active;
    }
    EXPECT_TRUE(shown) << "the CPU chart's tooltip";
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

// #1891: a panel that was never attached has no model to update, and onUpdate() leaves it at that.
TEST_F(SystemMetricsPanelRenderTest, UpdatingAPanelThatWasNeverAttachedDoesNothing)
{
    SystemMetricsPanel panel([] { return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr}; });
    panel.onUpdate(1.0F / 60.0F);
    EXPECT_FALSE(renderAndCapture(panel).contains("testhost")) << "nothing read, so no machine to show";
}

// #1395: hovering the power card's heading explains what it shows: the process power estimate, the
// battery's charge and its health, and how many samples the chart holds.
TEST_F(SystemMetricsPanelRenderTest, ThePowerHeadingSaysWhatTheChartShows)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        []
        {
            auto battery = mockBattery();
            Platform::PowerCounters counters;
            counters.state = Platform::BatteryState::Discharging;
            counters.chargePercent = 80;
            counters.powerNowW = 12.5;
            counters.healthPercent = 93;
            battery->setCounters(counters);
            return SystemMetricsProbes{.system = mockSystem(), .power = std::move(battery), .disk = nullptr, .gpu = nullptr};
        });
    attachAndSeed(*panel);
    const std::shared_ptr<Domain::ProcessModel> processes = mockProcessModel();
    panel->setProcessModel(processes);
    panel->setSamplingInterval(std::chrono::milliseconds(100));

    // Until the power card is up.
    const ImGuiWindow* card = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (card == nullptr && std::chrono::steady_clock::now() < deadline)
    {
        processes->refresh();
        panel->onUpdate(1.0F / 60.0F);
        if (renderAndCapture(*panel).contains("Power & Battery"))
        {
            for (const ImGuiWindow* window : GImGui->Windows)
            {
                if (std::string_view(window->Name).contains("##PowerCard") && window->Active)
                {
                    card = window;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_NE(card, nullptr);

    ASSERT_LT(card->Pos.y, DISPLAY_HEIGHT) << "the power card is below the window";

    // A tooltip waits for the pointer to rest (ImGuiHoveredFlags_ForTooltip): not here.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;

    // The fill layout settles over a few frames; the card's place is read once it has.
    for (int frame = 0; frame < 10; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
    }

    // Along the card's top, where its heading is, until the heading's tooltip shows.
    const ImVec2 cardPos = card->Pos;
    std::string text;
    for (int row = 0; row < 10 && !text.contains("Health:"); ++row)
    {
        for (int column = 0; column < 14 && !text.contains("Health:"); ++column)
        {
            ImGui::GetIO().AddMousePosEvent(cardPos.x + 10.0F + (15.0F * static_cast<float>(column)),
                                            cardPos.y + 2.0F + (4.0F * static_cast<float>(row)));
            panel->onUpdate(1.0F / 60.0F);
            static_cast<void>(renderAndCapture(*panel));
            text = renderAndCapture(*panel);
        }
    }
    EXPECT_TRUE(text.contains("Power: Aggregated CPU-proportional estimate")) << text;
    EXPECT_TRUE(text.contains("Battery: System battery charge")) << text;
    EXPECT_TRUE(text.contains("Health: 93")) << text;
    EXPECT_TRUE(text.contains("sample")) << text; // the sample count
    panel->onDetach();
}

// #1395: a GPU whose clock and fan this sample didn't read, and a system that reports no GPU temperature:
// the clock and the fan say they are unavailable this sample (the capability is there, the reading isn't)
// and the page notes the missing temperature.
TEST_F(SystemMetricsPanelRenderTest, TheGpuTabMarksReadingsItDoesntHave)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        []
        {
            auto gpu = std::make_unique<TestMocks::MockGPUProbe>();
            Platform::GPUCapabilities caps;
            caps.hasFanSpeed = true;
            caps.hasClockSpeeds = true;
            caps.hasTemperature = false;
            gpu->withCapabilities(caps);
            gpu->withGPU("GPU0", "Test GPU");
            Platform::GPUCounters counters = TestMocks::makeGPUCounters("GPU0");
            counters.gpuClockAvailable = false;
            counters.fanSpeedMaxRaw = 0; // the fan read failed this sample
            counters.temperatureAvailable = false;
            gpu->withGPUCounters("GPU0", counters);
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = std::move(gpu)};
        });
    attachAndSeed(*panel);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(selectTab("GPU"));

    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !text.contains("Fan : unavailable this sample"))
    {
        panel->onUpdate(1.0F / 60.0F);
        text = renderAndCapture(*panel);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(text.contains("Test GPU")) << text;
    EXPECT_TRUE(text.contains("Fan : unavailable this sample")) << text;
    EXPECT_TRUE(text.contains("Clock (% of 2000 MHz) : unavailable this sample")) << text;
    EXPECT_TRUE(text.contains("does not report GPU temperature")) << text;
    panel->onDetach();
}

// #1395: an interface whose link speed is unknown reads as a dash in the Interface Status table's Speed
// column, not "0 bit/s".
// #1891: a GPU that reports its memory in use but no total (a shared-memory device without a budget)
// still has a Memory bar -- its percent alone, not "unavailable".
TEST_F(SystemMetricsPanelRenderTest, AGpuMemoryReadingWithoutATotalStillHasItsBar)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        []
        {
            auto gpu = std::make_unique<TestMocks::MockGPUProbe>();
            gpu->withGPU("GPU0", "Test GPU");
            Platform::GPUCounters counters = TestMocks::makeGPUCounters("GPU0");
            counters.memoryUsedBytes = std::uint64_t{512} << 20U;
            counters.memoryTotalBytes = 0; // No total to set it against
            gpu->withGPUCounters("GPU0", counters);
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = std::move(gpu)};
        });
    attachAndSeed(*panel);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(selectTab("GPU"));
    std::string text;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int frames = 0; std::chrono::steady_clock::now() < deadline; ++frames)
    {
        panel->onUpdate(1.0F / 60.0F);
        text = renderAndCapture(*panel);
        if (frames > 20 && text.contains("Test GPU"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(text.contains("Test GPU")) << text;
    EXPECT_TRUE(text.contains("Memory")) << text;
    EXPECT_FALSE(text.contains("Memory : unavailable this sample")) << text;
    panel->onDetach();
}

TEST_F(SystemMetricsPanelRenderTest, AnInterfaceWithoutALinkSpeedSaysSo)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        [] { return SystemMetricsProbes{.system = mockSystem(true, "eth1", 0), .power = nullptr, .disk = nullptr, .gpu = nullptr}; });
    attachAndSeed(*panel);
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(selectTab("Network and I/O"));
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Interface Status")) << text;
    EXPECT_TRUE(text.contains("eth1")) << text;
    EXPECT_TRUE(text.contains("| eth1 | Up | - |")) << text; // Speed: unknown
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

/// The Overview's plot whose series are @p labels, or null.
[[nodiscard]] const ImPlotPlot* findPlot(const SeriesLabels& labels)
{
    ImPlotContext& context = *ImPlot::GetCurrentContext();
    int i = 0;
    while (i < context.Plots.GetBufSize())
    {
        ImPlotPlot* plot = context.Plots.GetByIndex(i);
        ++i;
        if (plot == nullptr)
        {
            continue;
        }
        SeriesLabels found;
        int item = 0;
        while (item < plot->Items.GetLegendCount())
        {
            found.emplace(plot->Items.GetLegendLabel(item));
            ++item;
        }
        if (found == labels)
        {
            return plot;
        }
    }
    return nullptr;
}

// #1916: the machine's handles (descriptors on Linux) no longer set the Threads line's axis. A machine
// holds tens to hundreds of times more handles than threads, so on one shared axis the Threads line
// and NowBar were flat at zero. The left axis now covers the threads alone, and the handles have a
// right-hand axis of their own (Y3), outside Page Faults' (Y2).
TEST_F(SystemMetricsPanelRenderTest, TheResourcesChartGivesHandlesTheirOwnAxis)
{
    constexpr std::int32_t SERVER_HANDLES = 100'000; // Against 9 threads in all
    constexpr double ALL_HANDLES = 12.0 + SERVER_HANDLES;
#ifdef _WIN32
    const SeriesLabels resources{"Threads", "Page Faults →", "Handles →"};
#else
    const SeriesLabels resources{"Threads", "Page Faults →", "FDs →"};
#endif
    const auto panel = std::make_unique<SystemMetricsPanel>(
        [] { return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr}; });
    attachAndSeed(*panel);
    const std::shared_ptr<Domain::ProcessModel> processes = mockProcessModel(SERVER_HANDLES);
    panel->setProcessModel(processes);
    panel->setSamplingInterval(std::chrono::milliseconds(100));

    // Until the handle count's axis has eased up to the samples.
    const ImPlotPlot* plot = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        processes->refresh();
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        plot = findPlot(resources);
        if (plot != nullptr && plot->Axes[ImAxis_Y3].Range.Max >= ALL_HANDLES)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_NE(plot, nullptr);
    const ImPlotAxis& threadAxis = plot->Axes[ImAxis_Y1];
    const ImPlotAxis& handleAxis = plot->Axes[ImAxis_Y3];
    ASSERT_TRUE(handleAxis.Enabled);
    EXPECT_TRUE(handleAxis.IsOpposite());
    EXPECT_GE(handleAxis.Range.Max, ALL_HANDLES);
    // The threads' axis is its smallest span, 10, with 9 threads near its top -- not 0 to 100K, where
    // they were a flat line on the baseline.
    EXPECT_DOUBLE_EQ(threadAxis.Range.Min, 0.0);
    EXPECT_LE(threadAxis.Range.Max, 20.0);
    EXPECT_GT(9.0 / threadAxis.Range.Size(), 0.4);
    panel->onDetach();
}

/// The vertices drawn in the theme's N/A NowBar hatch (UI::ColorScheme::unavailableHatch, #1935) in
/// the last frame. The stub scheme gives the hatch a colour nothing else uses, so any are an
/// unavailable bar's.
[[nodiscard]] int hatchVertices()
{
    const ImU32 hatch = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().unavailableHatch);
    int count = 0;
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
    {
        for (const ImDrawVert& vertex : list->VtxBuffer)
        {
            count += (vertex.col == hatch) ? 1 : 0;
        }
    }
    return count;
}

// #1935: a Resources total the process probe can't count at all -- threads, page faults or
// handles/descriptors -- has the hatched N/A bar on the Overview, as #1918 drew the Process Details
// ones; with every count read there is none.
TEST_F(SystemMetricsPanelRenderTest, AResourceTotalTheProbeCannotCountHasAnUnavailableBar)
{
    struct Case
    {
        const char* missing;
        void (*drop)(Platform::ProcessCapabilities&);
    };
    const std::array<Case, 4> cases{{
        {.missing = "nothing", .drop = [](Platform::ProcessCapabilities&) {}},
        {.missing = "threads", .drop = [](Platform::ProcessCapabilities& caps) { caps.hasThreadCount = false; }},
        {.missing = "page faults", .drop = [](Platform::ProcessCapabilities& caps) { caps.hasPageFaults = false; }},
        {.missing = "handles", .drop = [](Platform::ProcessCapabilities& caps) { caps.hasHandleCount = false; }},
    }};
    for (const Case& probeCase : cases)
    {
        SCOPED_TRACE(probeCase.missing);
        const auto panel = std::make_unique<SystemMetricsPanel>(
            [] { return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr}; });
        attachAndSeed(*panel);
        Platform::ProcessCapabilities caps = countingCapabilities();
        probeCase.drop(caps);
        const std::shared_ptr<Domain::ProcessModel> processes = mockProcessModel(40, caps);
        panel->setProcessModel(processes);
        panel->setSamplingInterval(std::chrono::milliseconds(100));

        // Until the process model's samples, and their capabilities, have reached the chart.
        const bool expectHatch = std::string_view(probeCase.missing) != "nothing";
        int hatch = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int frames = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            processes->refresh();
            panel->onUpdate(1.0F / 60.0F);
            const std::string text = renderAndCapture(*panel);
            hatch = hatchVertices();
            ++frames;
            if (frames > 30 && text.contains("Threads, Page Faults") && (hatch > 0) == expectHatch)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (expectHatch)
        {
            EXPECT_GT(hatch, 0);
        }
        else
        {
            EXPECT_EQ(hatch, 0);
        }
        panel->onDetach();
    }
}

// #1891: with an interface picked, its Network chart's tooltip lists the interface's rates beside the
// totals behind them, and the link speed explains itself as a byte rate on hover.
TEST_F(SystemMetricsPanelRenderTest, APickedInterfacesChartAndLinkSpeedShowTheirTooltips)
{
    const auto panel = attachedPanel(/*network=*/true);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    static_cast<void>(renderAndCapture(*panel));
    // The Overview's plots stay in ImPlot's pool once its tab is left, at their last place: only the
    // plots this tab draws are hovered below.
    std::set<ImGuiID> overviewPlots;
    for (int i = 0; i < ImPlot::GetCurrentContext()->Plots.GetBufSize(); ++i)
    {
        if (const ImPlotPlot* plot = ImPlot::GetCurrentContext()->Plots.GetByIndex(i); plot != nullptr)
        {
            overviewPlots.insert(plot->ID);
        }
    }
    ASSERT_TRUE(selectTab("Network and I/O"));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
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
    // History for the interface's chart: several samples.
    for (int frame = 0; frame < 40; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ImGuiIO& io = ImGui::GetIO();
    std::vector<ImRect> rects;
    ImPlotContext& plots = *ImPlot::GetCurrentContext();
    for (int i = 0; i < plots.Plots.GetBufSize(); ++i)
    {
        if (const ImPlotPlot* plot = plots.Plots.GetByIndex(i); plot != nullptr && !overviewPlots.contains(plot->ID) &&
                                                                plot->PlotRect.GetWidth() > 4.0F && plot->PlotRect.Max.y < DISPLAY_HEIGHT &&
                                                                plot->PlotRect.Min.y >= 0.0F)
        {
            rects.push_back(plot->PlotRect);
        }
    }
    ASSERT_FALSE(rects.empty());
    int tooltips = 0;
    for (const ImRect& rect : rects)
    {
        io.AddMousePosEvent(rect.Max.x - 3.0F, (rect.Min.y + rect.Max.y) * 0.5F);
        for (int frame = 0; frame < 3; ++frame)
        {
            panel->onUpdate(1.0F / 60.0F);
            static_cast<void>(renderAndCapture(*panel));
        }
        if (const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00"); tooltip != nullptr && tooltip->Active)
        {
            ++tooltips;
        }
    }
    // The Network chart shows one (its interface rows beside the totals); the Disk I/O chart beside it has
    // no samples here, with no disk probe, so nothing to show.
    EXPECT_GE(tooltips, 1) << "of " << rects.size() << " charts on the tab";

    // The link speed's line sits above the charts: sweep the section's top for its tooltip.
    bool byteRateShown = false;
    constexpr int SWEEP_STEP_PX = 4;
    for (int row = 0; row * SWEEP_STEP_PX < static_cast<int>(DISPLAY_HEIGHT * 0.5F) && !byteRateShown; ++row)
    {
        const auto y = static_cast<float>(row * SWEEP_STEP_PX);
        for (const float x : {40.0F, 120.0F, 200.0F, 300.0F})
        {
            io.AddMousePosEvent(x, y);
            static_cast<void>(renderAndCapture(*panel));
            if (renderAndCapture(*panel).contains("Up to "))
            {
                byteRateShown = true;
                break;
            }
        }
    }
    EXPECT_TRUE(byteRateShown) << "1 Gbit/s explains itself as a byte rate";
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

// #1891: with a disk probe, the Network and I/O tab's Disk I/O chart has samples too, and resting the
// pointer on each of the tab's charts shows its tooltip: Read/Write, and the Network chart's totals.
TEST_F(SystemMetricsPanelRenderTest, TheDiskAndNetworkChartsShowTheirTooltips)
{
    auto panel = std::make_unique<SystemMetricsPanel>(
        []
        {
            auto disk = std::make_unique<Mocks::MockDiskProbe>();
            Platform::SystemDiskCounters counters;
            Platform::DiskCounters sda;
            sda.deviceName = "sda";
            sda.readSectors = 2'000;
            sda.writeSectors = 1'000;
            counters.disks.push_back(sda);
            disk->setNextCounters(counters);
            return SystemMetricsProbes{.system = mockSystem(/*network=*/true), .power = nullptr, .disk = std::move(disk), .gpu = nullptr};
        });
    attachAndSeed(*panel);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    static_cast<void>(renderAndCapture(*panel));
    // The Overview's plots stay in ImPlot's pool at their last place: only this tab's are hovered.
    std::set<ImGuiID> overviewPlots;
    for (int i = 0; i < ImPlot::GetCurrentContext()->Plots.GetBufSize(); ++i)
    {
        if (const ImPlotPlot* plot = ImPlot::GetCurrentContext()->Plots.GetByIndex(i); plot != nullptr)
        {
            overviewPlots.insert(plot->ID);
        }
    }
    ASSERT_TRUE(selectTab("Network and I/O"));
    for (int frame = 0; frame < 40; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    ImGuiIO& io = ImGui::GetIO();
    std::vector<ImRect> rects;
    ImPlotContext& plots = *ImPlot::GetCurrentContext();
    for (int i = 0; i < plots.Plots.GetBufSize(); ++i)
    {
        if (const ImPlotPlot* plot = plots.Plots.GetByIndex(i); plot != nullptr && !overviewPlots.contains(plot->ID) &&
                                                                plot->PlotRect.GetWidth() > 4.0F && plot->PlotRect.Max.y < DISPLAY_HEIGHT &&
                                                                plot->PlotRect.Min.y >= 0.0F)
        {
            rects.push_back(plot->PlotRect);
        }
    }
    ASSERT_GE(rects.size(), 2U); // Disk I/O and Network
    int tooltips = 0;
    for (const ImRect& rect : rects)
    {
        io.AddMousePosEvent(rect.Max.x - 3.0F, (rect.Min.y + rect.Max.y) * 0.5F);
        for (int frame = 0; frame < 3; ++frame)
        {
            panel->onUpdate(1.0F / 60.0F);
            static_cast<void>(renderAndCapture(*panel));
        }
        if (const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00"); tooltip != nullptr && tooltip->Active)
        {
            ++tooltips;
        }
    }
    EXPECT_EQ(tooltips, static_cast<int>(rects.size())) << "every chart on the tab shows its tooltip";
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

// #2731: the sampler runs at the refresh setting in every UI state. Settings is the one thing that sets
// it: this tab being shown or another, frames at any rate, and the slow frames of a minimized window
// change nothing. (The System sampler once ran at twice the setting while its tab was hidden.)
TEST_F(SystemMetricsPanelRenderTest, TheSamplerIntervalIsTheRefreshSettingInEveryUiState)
{
    const auto panel = attachedPanel();
    const auto setting = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
    EXPECT_EQ(panel->samplingInterval(), setting);

    const auto expectThroughEveryUiState = [&panel](std::chrono::milliseconds expected)
    {
        // Each main tab shown in turn, with a frame on each: this panel's tab, and the others.
        for (const char* tab : {"SystemOverview", "Processes", "ProcessDetails", "Services", "Startup", "SystemInfo", "SystemOverview"})
        {
            Core::ActiveTabChangedEvent shown(tab);
            panel->onEvent(shown);
            panel->onUpdate(1.0F / 60.0F);
            EXPECT_EQ(panel->samplingInterval(), expected) << tab;
        }
        // Frames at the display rate of a resize or move, at 60 Hz, at the 5 Hz of a minimized window,
        // and after the gap of a window left minimized or covered.
        for (const float deltaTime : {0.002F, 1.0F / 60.0F, 0.2F, 2.0F})
        {
            for (int frame = 0; frame < 20; ++frame)
            {
                panel->onUpdate(deltaTime);
            }
            EXPECT_EQ(panel->samplingInterval(), expected) << deltaTime;
        }
    };
    expectThroughEveryUiState(setting);

    // Settings changes it, and the new value holds through the same states.
    Core::RefreshRateChangedEvent faster(250);
    panel->onEvent(faster);
    EXPECT_EQ(panel->samplingInterval(), std::chrono::milliseconds(250));
    expectThroughEveryUiState(std::chrono::milliseconds(250));

    Core::RefreshRateChangedEvent slower(Domain::Sampling::REFRESH_INTERVAL_MAX_MS);
    panel->onEvent(slower);
    EXPECT_EQ(panel->samplingInterval(), std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
    expectThroughEveryUiState(std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MAX_MS));
    panel->onDetach();
}

// #2731: onAttach() builds no probe and takes no read on the thread that calls it. The probe factory
// runs on the sampler thread, once, and the models are seeded there; the attaching thread finds the
// models in place (the shell wires them) but empty until the seed is adopted.
TEST_F(SystemMetricsPanelRenderTest, OnAttachLeavesTheProbeFactoryToTheSamplerThread)
{
    std::atomic<int> factoryCalls{0};
    std::atomic<std::thread::id> factoryThread{};
    auto panel = std::make_unique<SystemMetricsPanel>(
        [&factoryCalls, &factoryThread]
        {
            ++factoryCalls;
            factoryThread.store(std::this_thread::get_id());
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr};
        });
    panel->onAttach();
    EXPECT_NE(panel->gpuModel(), nullptr) << "the models exist from onAttach(), for the shell to wire";
    EXPECT_EQ(panel->hostname(), "System") << "no read has happened on this thread";

    ASSERT_TRUE(panel->waitForFirstSample());
    EXPECT_EQ(factoryCalls.load(), 1);
    EXPECT_NE(factoryThread.load(), std::this_thread::get_id()) << "the factory ran on the attaching thread";
    EXPECT_EQ(panel->hostname(), "testhost") << "the seed, taken on the sampler thread, was adopted";
    panel->onDetach();
}

// #2731: a frame drawn before the first publication -- the probes still being built on the sampler
// thread -- draws the Overview's chart frames with no data, not garbage and not a crash; the charts fill
// once the seed arrives.
TEST_F(SystemMetricsPanelRenderTest, TheFirstFrameDrawsWithoutASnapshot)
{
    auto gate = std::make_shared<std::latch>(1); // holds the factory, so no publication can arrive yet
    auto panel = std::make_unique<SystemMetricsPanel>(
        [gate]
        {
            gate->wait();
            return SystemMetricsProbes{.system = mockSystem(), .power = nullptr, .disk = nullptr, .gpu = nullptr};
        });
    panel->onAttach();
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string before = renderAndCapture(*panel);
    EXPECT_TRUE(before.contains("Overview")) << before;
    EXPECT_TRUE(before.contains("CPU Usage")) << before;
    EXPECT_FALSE(before.contains("CPU Cores")) << "no per-core readings yet: " << before;
    EXPECT_GE(ImPlot::GetCurrentContext()->Plots.GetBufSize(), 2) << "the chart frames are drawn empty";

    gate->count_down();
    ASSERT_TRUE(panel->waitForFirstSample());
    static_cast<void>(renderAndCapture(*panel));
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("CPU Cores")) << "the seed brought the mock's per-core readings: " << after;
    panel->onDetach();
}

} // namespace
} // namespace App
