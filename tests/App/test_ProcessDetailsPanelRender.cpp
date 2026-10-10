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

#include <cstdint>
#include <memory>
#include <string>
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
