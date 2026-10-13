/// @file test_ProcessDetailsPanelRender.cpp
/// @brief The Process Details panel as a whole (#1546), run headless with mock actions and readers
/// through its injectable constructor: nothing selected, a selected process's Overview (identity,
/// runtime, actions, the optional sections, the charts once it has history), the tab a selection asks
/// for, a process that leaves the list, and an inactive panel drawing nothing.

#include "App/Panels/ProcessDetailsPanel.h"
#include "App/SelectOverride.h"
#include "Core/ApplicationEvents.h"
#include "Domain/CrashHistory.h"
#include "Domain/ProcessSnapshot.h"
#include "Mocks/MockProbes.h"
#include "Platform/IProcessActions.h"
#include "Platform/ISystemInfoProbe.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;
constexpr std::int32_t PID = 4242;
constexpr std::uint64_t START_TICKS = 777;

/// One sample holding a single process: "server", PID 4242, at @p seconds with @p cpuPercent.
[[nodiscard]] Domain::ProcessSample sampleAt(double seconds, double cpuPercent)
{
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>();
    snapshot->pid = PID;
    snapshot->parentPid = 1;
    snapshot->startTimeTicks = START_TICKS;
    snapshot->uniqueKey = 99;
    snapshot->name = "server";
    snapshot->command = "/usr/bin/server --port 8080";
    snapshot->user = "svc";
    snapshot->displayState = "Running";
    snapshot->threadCount = 4;
    snapshot->handleCount = 12;
    snapshot->cpuPercent = cpuPercent;
    snapshot->cpuUserPercent = cpuPercent * 0.75;
    snapshot->cpuSystemPercent = cpuPercent * 0.25;
    snapshot->memoryBytes = std::uint64_t{64} << 20U;
    snapshot->virtualBytes = std::uint64_t{512} << 20U;
    snapshot->cpuTimeSeconds = seconds;

    Domain::ProcessSample sample;
    sample.snapshot = std::move(snapshot);
    sample.version = static_cast<std::uint64_t>(seconds) + 1;
    sample.sampleTimeSeconds = seconds;
    return sample;
}

/// A sample with no processes at all: the selected one has exited.
[[nodiscard]] Domain::ProcessSample emptySampleAt(double seconds)
{
    Domain::ProcessSample sample;
    sample.version = static_cast<std::uint64_t>(seconds) + 1;
    sample.sampleTimeSeconds = seconds;
    return sample;
}

class ProcessDetailsPanelRenderTest : public ::testing::Test
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

    /// A panel with mock actions and environment, connections, modules and security readers, shown as
    /// the active tab.
    [[nodiscard]] static std::unique_ptr<ProcessDetailsPanel> activePanel()
    {
        auto actions = std::make_unique<TestMocks::MockProcessActions>();
        Platform::ProcessActionCapabilities caps;
        caps.canTerminate = true;
        caps.canKill = true;
        actions->setCapabilities(caps);
        auto panel = std::make_unique<ProcessDetailsPanel>(std::move(actions),
                                                           std::make_unique<TestMocks::MockProcessEnvironmentReader>(),
                                                           std::make_unique<TestMocks::MockProcessConnectionsReader>(),
                                                           std::make_unique<TestMocks::MockProcessModulesReader>(),
                                                           std::make_unique<TestMocks::MockProcessSecurityReader>());
        Core::ActiveTabChangedEvent shown("ProcessDetails");
        panel->onEvent(shown);
        return panel;
    }

    /// One frame of a window filling the display, drawing @p panel's content, and the text it drew.
    [[nodiscard]] static std::string renderAndCapture(ProcessDetailsPanel& panel)
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

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(ProcessDetailsPanelRenderTest, NothingSelectedSaysSo)
{
    const auto panel = activePanel();
    EXPECT_TRUE(renderAndCapture(*panel).contains("No process selected"));
}

TEST_F(ProcessDetailsPanelRenderTest, ASelectedProcessShowsItsOverview)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 5; ++second)
    {
        const std::vector samples{sampleAt(static_cast<double>(second), 10.0 + second)};
        panel->updateWithSamples(samples, 1.0F);
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);

    EXPECT_TRUE(text.contains("Overview")) << text;
    EXPECT_TRUE(text.contains("Identity")) << text;
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("4242")) << text;
    EXPECT_TRUE(text.contains("svc")) << text;
    EXPECT_TRUE(text.contains("Runtime")) << text;
    EXPECT_TRUE(text.contains("Actions")) << text;
    // Each injected reader's section, collapsed until opened.
    EXPECT_TRUE(text.contains("Environment")) << text;
    EXPECT_TRUE(text.contains("Connections")) << text;
    EXPECT_TRUE(text.contains("Modules")) << text;
    EXPECT_TRUE(text.contains("Security")) << text;
    // The charts once the process has history.
    EXPECT_TRUE(text.contains("CPU")) << text;
    EXPECT_TRUE(text.contains("Memory")) << text;
    EXPECT_GE(ImPlot::GetCurrentContext()->Plots.GetBufSize(), 2);
}

// #1395: the optional identity and runtime rows -- Publisher (Windows), Type (Windows) and Container
// (Linux, #805) -- are shown when the process has them.
TEST_F(ProcessDetailsPanelRenderTest, TheOptionalRowsShowWhenTheProcessHasThem)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    Domain::ProcessSample sample = sampleAt(1.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->publisher = "Example Corp";
    snapshot->processType = "Background Process";
    snapshot->container = "docker:1a2b3c4d5e6f";
    sample.snapshot = std::move(snapshot);
    const std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Publisher")) << text;
    EXPECT_TRUE(text.contains("Example Corp")) << text;
    EXPECT_TRUE(text.contains("Background Process")) << text;
    EXPECT_TRUE(text.contains("Container")) << text;
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f")) << text;
}

// #1395: a shorter history window trims the history already held, so the charts span the new window.
TEST_F(ProcessDetailsPanelRenderTest, AShorterHistoryWindowTrimsTheCharts)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 120; second += 2)
    {
        const std::vector samples{sampleAt(static_cast<double>(second), 10.0)};
        panel->updateWithSamples(samples, 1.0F);
    }
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImPlotPlot* cpu = ImPlot::GetCurrentContext()->Plots.GetByIndex(0);
    ASSERT_NE(cpu, nullptr);
    const double before = cpu->Axes[ImAxis_X1].Range.Size();

    Core::HistoryDurationChangedEvent shorter(30);
    panel->onEvent(shorter);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const double after = cpu->Axes[ImAxis_X1].Range.Size();
    EXPECT_LT(after, before);
    EXPECT_NEAR(after, 30.0, 1.0);
}

// #1395: F9 (requestKillSelected()) opens the Kill confirmation for the process shown, without
// killing it.
TEST_F(ProcessDetailsPanelRenderTest, TheKillShortcutAsksFirst)
{
    auto actions = std::make_unique<TestMocks::MockProcessActions>();
    Platform::ProcessActionCapabilities caps;
    caps.canTerminate = true;
    caps.canKill = true;
    actions->setCapabilities(caps);
    const TestMocks::MockProcessActions* mock = actions.get();
    ProcessDetailsPanel panel(std::move(actions));
    Core::ActiveTabChangedEvent shown("ProcessDetails");
    panel.onEvent(shown);
    panel.setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(1.0, 10.0)};
    panel.updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(panel));

    panel.requestKillSelected();
    static_cast<void>(renderAndCapture(panel));
    static_cast<void>(renderAndCapture(panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Kill")) << modal->Name;
    EXPECT_EQ(mock->killCount(), 0); // asked, not done
}

TEST_F(ProcessDetailsPanelRenderTest, TheOverviewShowsTheExecutablesRecentCrashes)
{
    // Without a crash history (synthetic runs, the other tests here) there is no line at all.
    const auto bare = activePanel();
    bare->setSelectedPid(PID, START_TICKS);
    const std::vector first{sampleAt(0.0, 10.0)};
    bare->updateWithSamples(first, 1.0F);
    EXPECT_FALSE(renderAndCapture(*bare).contains("Recent crashes")) << "no history, no line";

    // With one, already read (as the System tab's read publishes it), the selected executable's own.
    int reads = 0;
    const auto history = std::make_shared<Domain::CrashHistory>(
        [&reads]
        {
            ++reads;
            return Platform::CrashesInfo{};
        });
    Platform::CrashesInfo crashes;
    crashes.available = true;
    crashes.listed = true;
    crashes.family = Platform::OsFamily::Linux;
    const auto crashOf = [](const char* application, std::uint64_t unixSeconds)
    {
        Platform::CrashEvent event;
        event.application = application;
        event.unixSeconds = unixSeconds;
        return event;
    };
    crashes.events = {crashOf("server", 1'790'000'000), crashOf("client", 1'789'000'000), crashOf("server", 1'788'000'000)};
    history->publish(std::move(crashes), 1'790'000'100);

    const auto panel = activePanel();
    panel->setCrashHistory(history);
    panel->setSelectedPid(PID, START_TICKS);
    panel->updateWithSamples(first, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Recent crashes:")) << text;
    EXPECT_TRUE(text.contains("2 crashes in the last 14 days")) << text;
    // Fresh, so drawing it read nothing more.
    panel->updateWithSamples({}, 1.0F);
    EXPECT_EQ(reads, 0);
}

TEST_F(ProcessDetailsPanelRenderTest, ARequestedTabIsSelected)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(samples, 1.0F);
    panel->requestTab(SelectOverride::DetailsTab::Network);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    // The Network and I/O tab's charts, not the Overview.
    EXPECT_TRUE(text.contains("I/O Statistics")) << text;
    EXPECT_TRUE(text.contains("Sent")) << text;
    EXPECT_FALSE(text.contains("Identity")) << text;
}

/// The labels of the tabs Process Details' tab bar drew.
[[nodiscard]] std::vector<std::string> detailsTabLabels()
{
    std::vector<std::string> labels;
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
            labels.emplace_back(ImGui::TabBarGetTabName(bar, &tab));
        }
    }
    return labels;
}

/// A sample of a process that uses the NPU, as an Intel AI Boost user does.
[[nodiscard]] Domain::ProcessSample npuSampleAt(double seconds)
{
    Domain::ProcessSample sample = sampleAt(seconds, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->gpuFieldsRead = true;
    snapshot->npuUtilPercent = 25.0;
    snapshot->npuMemoryBytes = 4'194'304;
    snapshot->npuSharedMemoryBytes = 4'194'304;
    snapshot->npuDevices = "Intel(R) AI Boost";
    snapshot->npuEngines = {"Compute"};
    sample.snapshot = std::move(snapshot);
    return sample;
}

// #1869: the NPU tab sits after the GPU tab only on a machine with an NPU or compute accelerator, as
// the machine view's does; asked for without one, nothing NPU is drawn and the Overview stays.
TEST_F(ProcessDetailsPanelRenderTest, TheNpuTabAppearsOnlyWithAnAccelerator)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{npuSampleAt(0.0), npuSampleAt(1.0)};
    panel->updateWithSamples(samples, 1.0F);

    panel->requestTab(SelectOverride::DetailsTab::Npu);
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    std::vector<std::string> tabs = detailsTabLabels();
    EXPECT_FALSE(std::ranges::any_of(tabs, [](const std::string& label) { return label.contains("NPU"); })) << text;
    EXPECT_FALSE(text.contains("NPU")) << text;
    EXPECT_TRUE(text.contains("Identity")) << text;

    panel->setHasAccelerator(true);
    panel->requestTab(SelectOverride::DetailsTab::Npu);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    tabs = detailsTabLabels();
    const auto gpuTab = std::ranges::find_if(tabs, [](const std::string& label) { return label.contains("GPU"); });
    const auto npuTab = std::ranges::find_if(tabs, [](const std::string& label) { return label.contains("NPU"); });
    ASSERT_NE(npuTab, tabs.end()) << text;
    ASSERT_NE(gpuTab, tabs.end()) << text;
    EXPECT_EQ(std::ranges::distance(gpuTab, npuTab), 1) << "The NPU tab is next to the GPU tab";
    EXPECT_TRUE(text.contains("NPU Usage")) << text;
    EXPECT_TRUE(text.contains("Intel(R) AI Boost")) << text;
    EXPECT_FALSE(text.contains("Identity")) << text;

    panel->setHasAccelerator(false); // The accelerator goes away: so does the tab
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_FALSE(text.contains("NPU Usage")) << text;
}

TEST_F(ProcessDetailsPanelRenderTest, AProcessThatLeavesTheListIsReportedExited)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector first{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(first, 1.0F);
    static_cast<void>(renderAndCapture(*panel));

    const std::vector gone{emptySampleAt(1.0)};
    panel->updateWithSamples(gone, 1.0F);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Process exited")) << text;
    EXPECT_TRUE(text.contains("server (PID 4242) is no longer running")) << text;
}

TEST_F(ProcessDetailsPanelRenderTest, AnInactivePanelDrawsNothing)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(samples, 1.0F);
    Core::ActiveTabChangedEvent away("Processes");
    panel->onEvent(away);
    EXPECT_TRUE(renderAndCapture(*panel).empty());
}

} // namespace
} // namespace App
