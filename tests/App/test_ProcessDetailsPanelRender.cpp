/// @file test_ProcessDetailsPanelRender.cpp
/// @brief The Process Details panel as a whole (#1546), run headless with mock actions and readers
/// through its injectable constructor: nothing selected, a selected process's Overview (identity,
/// runtime, actions, the optional sections, the charts once it has history), the tab a selection asks
/// for, a process that leaves the list, and an inactive panel drawing nothing. The Actions block's
/// dialogs stay submitted whatever the pane shows: a confirm closes when its process exits or another is
/// selected, and a dump in flight keeps its progress and then shows its result (#2117). The I/O slider
/// follows capabilities that change after construction, as Realtime does once the helper connects (#2407).

#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessDetailsPanel.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessIoPriorityView.h"
#include "App/Panels/ProcessPriorityView.h"
#include "App/SelectOverride.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/CrashHistory.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/ServiceModel.h"
#include "Mocks/FakeBroker.h"
#include "Mocks/MockProbes.h"
#include "Platform/BrokeredProcessActions.h"
#include "Platform/CpuAffinity.h"
#include "Platform/IProcessActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ISystemInfoProbe.h"
#include "Platform/ProcessTypes.h"
#include "UI/Format.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <implot_internal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iterator>
#include <memory>
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

// #2475: a setuid program run by another user says who started it beside who it runs as; others don't.
TEST_F(ProcessDetailsPanelRenderTest, ASetuidProcessSaysWhoStartedIt)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector plain{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(plain, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Started by"));

    Domain::ProcessSample setuid = sampleAt(1.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*setuid.snapshot);
    snapshot->user = "root";
    snapshot->realUser = "alice";
    setuid.snapshot = snapshot;
    const std::vector samples{setuid};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Started by")) << text;
    EXPECT_TRUE(text.contains("alice")) << text;
}

TEST_F(ProcessDetailsPanelRenderTest, NothingSelectedSaysSo)
{
    const auto panel = activePanel();
    EXPECT_TRUE(renderAndCapture(*panel).contains("No process selected"));
}

// #3156: while the hidden window draws no frames, ShellPanels::updateWhileHidden() hands the selected
// process's samples to takeSamples() alone. Each one reaches the pane, as through updateWithSamples(),
// so the next frame draws them; an exit is noticed the same way.
TEST_F(ProcessDetailsPanelRenderTest, SamplesTakenWithoutAFrameAreTheOnesTheNextFrameShows)
{
    const auto panel = activePanel();
    const std::vector before{sampleAt(0.0, 10.0)};
    panel->takeSamples(before); // nothing selected yet: nothing taken
    EXPECT_EQ(panel->lastSampleVersion(), 0U);
    EXPECT_EQ(panel->displayedSnapshot(), nullptr);

    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 5; ++second)
    {
        const std::vector samples{sampleAt(static_cast<double>(second), 10.0 + second)};
        panel->takeSamples(samples);
    }
    EXPECT_EQ(panel->lastSampleVersion(), 5U);
    ASSERT_NE(panel->displayedSnapshot(), nullptr);
    EXPECT_DOUBLE_EQ(panel->displayedSnapshot()->cpuTimeSeconds, 4.0);
    EXPECT_TRUE(panel->showsRunningProcess());

    // The next frame has nothing new to take, and draws the charts from what was taken.
    panel->updateWithSamples({}, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("CPU")) << text;
    EXPECT_GE(ImPlot::GetCurrentContext()->Plots.GetBufSize(), 2);

    const std::vector exited{emptySampleAt(5.0)};
    panel->takeSamples(exited);
    EXPECT_EQ(panel->lastSampleVersion(), 6U);
    EXPECT_FALSE(panel->showsRunningProcess());
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

// #1926: the Details tab is named after the process; with several processes selected in the table,
// often some of one name, it gives the PID too, so it says which of them it shows.
TEST_F(ProcessDetailsPanelRenderTest, TheTabLabelNamesThePidWhileSeveralAreSelected)
{
    const auto panel = activePanel();
    EXPECT_EQ(panel->tabLabel(), "Select a process");
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(samples, 1.0F);
    panel->setSelectionCount(1);
    EXPECT_EQ(panel->tabLabel(), "server");
    panel->setSelectionCount(3);
    EXPECT_EQ(panel->tabLabel(), "server (PID 4242)");
    EXPECT_EQ(panel->tabLabel(), "server (PID 4242)"); // Asked again, the same text
    panel->setSelectionCount(1);
    EXPECT_EQ(panel->tabLabel(), "server");
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
    snapshot->services = "nginx.service";      // #1886
    snapshot->architecture = "x64 (emulated)"; // #2394: the Runtime card's eighth row, with Type and Container
    sample.snapshot = std::move(snapshot);
    const std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Architecture")) << text;
    EXPECT_TRUE(text.contains("x64 (emulated)")) << text;
    EXPECT_TRUE(text.contains("Publisher")) << text;
    EXPECT_TRUE(text.contains("Example Corp")) << text;
    EXPECT_TRUE(text.contains("Background Process")) << text;
    EXPECT_TRUE(text.contains("Container")) << text;
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f")) << text;
    EXPECT_TRUE(text.contains("Services")) << text;
    EXPECT_TRUE(text.contains("nginx.service")) << text;
}

// #1878: what the Processes table shows is on Process Details too: Status joined to State, Affinity in
// Identity, the cumulative Page Faults and Mem % in the Resources and Memory value strips; each only where
// the platform reads it.
TEST_F(ProcessDetailsPanelRenderTest, TheTablesStatusAffinityFaultsAndMemPercentAreShown)
{
    const auto panel = activePanel();
    Platform::ProcessCapabilities caps;
    caps.hasStatus = true;
    caps.hasCpuAffinity = true;
    caps.hasPageFaults = true;
    caps.hasPrivateBytes = true;
    caps.hasSwapBytes = true;
    panel->setProcessCapabilities(caps);
    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 5; ++second)
    {
        Domain::ProcessSample sample = sampleAt(static_cast<double>(second), 10.0);
        auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
        snapshot->status = "Suspended";
        snapshot->cpuAffinity = Platform::CpuAffinity::fromMask(0x0FU);
        snapshot->pageFaults = 4321;
        snapshot->memoryPercent = 1.5;
        snapshot->privateBytes = std::uint64_t{48} << 20U; // #1885
        snapshot->swapBytes = std::uint64_t{3} << 20U;
        sample.snapshot = std::move(snapshot);
        const std::vector samples{sample};
        panel->updateWithSamples(samples, 1.0F);
    }
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Running \xC2\xB7 Suspended")) << text;
    EXPECT_TRUE(text.contains("Affinity")) << text;
    EXPECT_TRUE(text.contains("0-3")) << text;
    EXPECT_TRUE(text.contains("Page Faults (total)")) << text;
    EXPECT_TRUE(text.contains("4,321") || text.contains("4321")) << text;
    EXPECT_TRUE(text.contains("Mem %")) << text;
    EXPECT_TRUE(text.contains("1.5%")) << text;
    EXPECT_TRUE(text.contains("Private")) << text;
    EXPECT_TRUE(text.contains("48.0 MiB")) << text;
    EXPECT_TRUE(text.contains("Swap")) << text;
    EXPECT_TRUE(text.contains("3.0 MiB")) << text;

    // Where the platform reads none of them: no Affinity row, no total, the State alone.
    panel->setProcessCapabilities({});
    Domain::ProcessSample later = sampleAt(5.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*later.snapshot);
    snapshot->status = "Suspended";
    later.snapshot = std::move(snapshot);
    const std::vector samples{later};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_FALSE(text.contains("Suspended")) << text;
    EXPECT_FALSE(text.contains("Affinity")) << text;
    EXPECT_FALSE(text.contains("Page Faults (total)")) << text;
    EXPECT_FALSE(text.contains("Swap")) << text;
}

// #1885: PSS and USS, read for the process Process Details shows (ProcessSnapshot::memoryRollupRead), are
// in its Memory value strip; a sample without them has neither.
// #1891: an Overview value wider than its column (a long affinity list, #1878) is clipped, and its
// tooltip has it whole.
TEST_F(ProcessDetailsPanelRenderTest, AClippedOverviewValueShowsWholeOnHover)
{
    const auto panel = activePanel();
    Platform::ProcessCapabilities caps;
    caps.hasCpuAffinity = true;
    panel->setProcessCapabilities(caps);
    panel->setSelectedPid(PID, START_TICKS);
    // Every other CPU of 512: "0,2,4,...,510", far wider than the value column at any display width.
    std::string list;
    for (int cpu = 0; cpu < 512; cpu += 2)
    {
        list += (cpu == 0 ? "" : ",") + std::to_string(cpu);
    }
    const Platform::CpuAffinity everyOther = Platform::CpuAffinity::fromCpuList(list).value_or(Platform::CpuAffinity{});
    ASSERT_FALSE(everyOther.empty());
    Domain::ProcessSample sample = sampleAt(0.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->cpuAffinity = everyOther;
    sample.snapshot = std::move(snapshot);
    const std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));

    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    const std::string whole = UI::Format::formatCpuAffinity(everyOther.words());
    // Down the middle of every card table's Value column (the clipped value fills its column), so the
    // card's position doesn't matter and few frames are drawn.
    std::vector<std::pair<float, ImRect>> valueColumns; // x, the table's rows
    for (int t = 0; t < GImGui->Tables.GetMapSize(); ++t)
    {
        const ImGuiTable* table = GImGui->Tables.TryGetMapData(t);
        for (int c = 0; table != nullptr && c < table->ColumnsCount; ++c)
        {
            if (std::string_view(ImGui::TableGetColumnName(table, c)) == "Value")
            {
                valueColumns.emplace_back((table->Columns[c].MinX + table->Columns[c].MaxX) * 0.5F, table->InnerRect);
            }
        }
    }
    ASSERT_FALSE(valueColumns.empty()) << "no Label/Value table drawn";
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& [x, rows] : valueColumns)
    {
        const int steps = static_cast<int>(rows.GetHeight() / 4.0F);
        for (int step = 0; step < steps && !shown; ++step)
        {
            io.AddMousePosEvent(x, rows.Min.y + 2.0F + (static_cast<float>(step) * 4.0F));
            for (int settle = 0; settle < 2 && !shown; ++settle)
            {
                const std::string hovered = renderAndCapture(*panel);
                const ImGuiWindow* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                shown = tooltip != nullptr && tooltip->Active && hovered.contains(whole);
            }
        }
    }
    EXPECT_TRUE(shown) << "the affinity row's tooltip: " << whole;
}

TEST_F(ProcessDetailsPanelRenderTest, PssAndUssAreShownWhenTheyWereRead)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    Domain::ProcessSample sample = sampleAt(1.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->pssBytes = std::uint64_t{20} << 20U;
    snapshot->ussBytes = std::uint64_t{12} << 20U;
    snapshot->memoryRollupRead = true;
    sample.snapshot = std::move(snapshot);
    std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("PSS")) << text;
    EXPECT_TRUE(text.contains("20.0 MiB")) << text;
    EXPECT_TRUE(text.contains("USS")) << text;
    EXPECT_TRUE(text.contains("12.0 MiB")) << text;

    samples = {sampleAt(2.0, 10.0)}; // Not read for this one
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_FALSE(text.contains("PSS")) << text;
    EXPECT_FALSE(text.contains("USS")) << text;
}

// #1879: on Windows the Memory strip has neither Private (its private bytes are the commit size, the
// Virtual line) nor Swap (no per-process figure): both are left out, as Shared is, not shown as 0.
TEST_F(ProcessDetailsPanelRenderTest, WindowsShowsNeitherPrivateNorSwap)
{
    const auto panel = activePanel();
    Platform::ProcessCapabilities caps;
    caps.hasPrivateBytes = false;
    caps.hasSwapBytes = false;
    panel->setProcessCapabilities(caps);
    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 5; ++second)
    {
        Domain::ProcessSample sample = sampleAt(static_cast<double>(second), 10.0);
        auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
        snapshot->privateBytes = std::uint64_t{72} << 20U;
        sample.snapshot = std::move(snapshot);
        const std::vector samples{sample};
        panel->updateWithSamples(samples, 1.0F);
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Memory")) << text;
    EXPECT_FALSE(text.contains("Private")) << text;
    EXPECT_FALSE(text.contains("72.0 MiB")) << text;
    EXPECT_FALSE(text.contains("Swap")) << text;
}

// #1886: a process that runs a service has a collapsed Services section whose "Show in Services" asks the
// shell to show that service; one that runs none has no section.
TEST_F(ProcessDetailsPanelRenderTest, TheServicesSectionShowsTheServiceInTheServicesTab)
{
    const auto panel = activePanel();
    std::vector<std::string> shown;
    panel->setEventSinkForTesting(
        [&shown](Core::Event& event)
        {
            Core::EventDispatcher dispatcher(event);
            dispatcher.dispatch<Core::ShowServiceEvent>(
                [&shown](Core::ShowServiceEvent& show)
                {
                    shown.push_back(show.name());
                    return false;
                });
        });
    panel->setSelectedPid(PID, START_TICKS);
    Domain::ProcessSample sample = sampleAt(1.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->services = "nginx.service";
    sample.snapshot = std::move(snapshot);
    const std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Services")) << text;

    // Open the section, then press its button.
    ImGuiWindow* content = nullptr;
    for (ImGuiWindow* window : GImGui->Windows)
    {
        // The Overview's own child, not the cards nested in it ("##OverviewContent_.../BasicInfoLeft_...").
        const std::string_view name(window->Name);
        if (const std::size_t at = name.find("##OverviewContent");
            at != std::string_view::npos && name.find('/', at) == std::string_view::npos)
        {
            content = window;
        }
    }
    ASSERT_NE(content, nullptr);
    // The sections sit under the charts, out of view; a clipped item can't be activated.
    ImGui::SetScrollY(content, content->ScrollMax.y);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::SetScrollY(content, content->ScrollMax.y);
    static_cast<void>(renderAndCapture(*panel));
    // Opened as a click leaves it: the header's open state in its window's storage.
    content->StateStorage.SetInt(ImHashStr("###ProcessServices", 0, content->ID), 1);
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("Show in Services")) << text;
    const ImGuiID button = ImHashStr(ICON_FA_ARROW_RIGHT "  Show in Services", 0, ImHashStr("nginx.service", 0, content->ID));
    ImGui::SetScrollY(content, content->ScrollMax.y);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::ActivateItemByID(button);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(shown, (std::vector<std::string>{"nginx.service"}));
}

// #1880: on Windows the section takes each service's display name and state from the service list, and
// while it is open it asks for that list to be kept current.
TEST_F(ProcessDetailsPanelRenderTest, TheServicesSectionShowsTheListsDisplayNamesAndStates)
{
    const auto panel = activePanel();
    Platform::ServiceCapabilities listCaps;
    listCaps.canEnumerate = true;
    listCaps.hasPid = true;
    listCaps.hasDisplayName = true;
    auto probe = std::make_unique<TestMocks::MockServiceProbe>(listCaps);
    Platform::ServiceInfo dhcp;
    dhcp.name = "Dhcp";
    dhcp.displayName = "DHCP Client";
    dhcp.state = Platform::ServiceState::Running;
    dhcp.pid = PID;
    probe->setEnumeration(Platform::ServiceEnumeration::succeeded({dhcp}));
    const auto serviceModel = std::make_shared<Domain::ServiceModel>(std::move(probe));
    serviceModel->sample();
    panel->setServiceModel(serviceModel);

    panel->setSelectedPid(PID, START_TICKS);
    Domain::ProcessSample sample = sampleAt(1.0, 10.0);
    auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
    snapshot->serviceNames = {"Dhcp"};
    snapshot->services = "Dhcp";
    sample.snapshot = std::move(snapshot);
    const std::vector samples{sample};
    panel->updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("DHCP Client")); // collapsed
    panel->updateWithSamples({}, 1.0F);
    EXPECT_FALSE(panel->servicesSectionOpen());

    ImGuiWindow* content = nullptr;
    int windowIndex = 0;
    while (windowIndex < GImGui->Windows.Size)
    {
        const std::string_view name(GImGui->Windows[windowIndex]->Name);
        if (const std::size_t at = name.find("##OverviewContent");
            at != std::string_view::npos && name.find('/', at) == std::string_view::npos)
        {
            content = GImGui->Windows[windowIndex];
        }
        ++windowIndex;
    }
    ASSERT_NE(content, nullptr);
    content->StateStorage.SetInt(ImHashStr("###ProcessServices", 0, content->ID), 1);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("DHCP Client")) << text;
    EXPECT_TRUE(text.contains("Running")) << text;
    panel->updateWithSamples({}, 1.0F);
    EXPECT_TRUE(panel->servicesSectionOpen()); // open: ShellLayer keeps the list sampled
}

// #1891: where the platform measures power, the Overview charts it: the Power card with its plot and value.
TEST_F(ProcessDetailsPanelRenderTest, ThePowerChartIsDrawnWherePowerIsMeasured)
{
    const auto panel = activePanel();
    Platform::ProcessCapabilities caps;
    caps.hasPowerUsage = true;
    panel->setProcessCapabilities(caps);
    panel->setSelectedPid(PID, START_TICKS);
    for (int second = 0; second < 5; ++second)
    {
        Domain::ProcessSample sample = sampleAt(static_cast<double>(second), 10.0);
        auto snapshot = std::make_shared<Domain::ProcessSnapshot>(*sample.snapshot);
        snapshot->powerWatts = 2.5 + second;
        sample.snapshot = std::move(snapshot);
        const std::vector samples{sample};
        panel->updateWithSamples(samples, 1.0F);
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Power Usage")) << text;
    EXPECT_TRUE(text.contains(" W")) << "a watts value (smoothed, so not the raw reading): " << text;

    // Where it isn't measured, there is no Power card.
    panel->setProcessCapabilities({});
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Power Usage"));
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

// #2407: the Realtime I/O class can become settable after the panel is built -- BrokeredProcessActions
// reports it once the privileged helper connects -- and the I/O slider then offers its Realtime stops.
// They used to come from a copy of the capabilities taken at construction, so they never appeared.
TEST_F(ProcessDetailsPanelRenderTest, TheIoSliderGainsRealtimeOnceTheActionsCanSetIt)
{
    auto actions = std::make_unique<TestMocks::MockProcessActions>();
    Platform::ProcessActionCapabilities caps;
    caps.canSetPriority = true;
    caps.canSetIoPriority = true;
    caps.canAuthorizeInteractively = true;
    actions->setCapabilities(caps); // No Realtime yet: the helper is not connected
    actions->setIoPriorityReadResult(Platform::IoPriority{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 0});
    TestMocks::MockProcessActions* mock = actions.get();
    ProcessDetailsPanel panel(std::move(actions));
    Core::ActiveTabChangedEvent shown("ProcessDetails");
    panel.onEvent(shown);
    panel.setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(1.0, 10.0)};
    panel.updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(panel));
    ASSERT_FALSE(panel.actionCapabilities().canSetRealtimeIoPriority);

    // The update path after the card was first drawn reads the current I/O priority on its worker
    // (#2734); taken in here, so the slider below starts from Best-effort 0.
    panel.updateWithSamples(samples, 1.0F);
    (void) panel.priorityViewForTesting().updateReads(mock,
                                                      Platform::ProcessTarget{.pid = PID, .startTimeTicks = START_TICKS},
                                                      0.0F,
                                                      /*wait=*/true);
    ASSERT_EQ(mock->getIoPriorityCount(), 1);

    caps.canSetRealtimeIoPriority = true; // The helper connected
    mock->setCapabilities(caps);
    static_cast<void>(renderAndCapture(panel));
    EXPECT_TRUE(panel.actionCapabilities().canSetRealtimeIoPriority);

    // One step toward the high end from Best-effort 0 is Realtime 7 on the slider with Realtime; the
    // slider without it holds at its start.
    const ImGuiWindow* card = nullptr;
    for (const ImGuiWindow* window : ImGui::GetCurrentContext()->Windows)
    {
        if ((window->Flags & ImGuiWindowFlags_ChildWindow) != 0 && std::string_view{window->Name}.contains("ProcessPriorityBlock_"))
        {
            card = window;
        }
    }
    ASSERT_NE(card, nullptr);
    ImGuiWindow* cardForFocus = ImGui::FindWindowByID(card->ID);
    const ImGuiID sliderId = ImHashStr(Detail::IO_PRIORITY_SLIDER_ID, 0, ImHashStr("io_priority", 0, card->ID));
    // Focused as the slider's own tests do it, at the start of a frame that draws it.
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
    ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetFocusID(sliderId, cardForFocus);
    panel.renderContent();
    ImGui::End();
    ImGui::Render();
    static_cast<void>(renderAndCapture(panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, true);
    static_cast<void>(renderAndCapture(panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_LeftArrow, false);
    static_cast<void>(renderAndCapture(panel));
    const Platform::IoPriority picked = panel.priorityViewForTesting().ioPriorityView().shownIoPriority();
    EXPECT_EQ(picked.ioClass, Platform::IoPriorityClass::Realtime);
    EXPECT_EQ(picked.level, 7);
    EXPECT_EQ(mock->setIoPriorityCount(), 0); // Picking applies nothing
}

// #3182: the panel asks for the actions' capabilities every frame, on the UI thread. Through the helper's
// broker that read the helper's connection as last seen, never connected(), which peeks at the pipe or
// polls the socket: no frame makes a call on the channel, while the Realtime stops still follow the helper.
TEST_F(ProcessDetailsPanelRenderTest, EveryFramesCapabilitiesReadTheHelpersConnectionWithoutAskingTheBroker)
{
    auto inner = std::make_unique<TestMocks::MockProcessActions>();
    inner->setCapabilities({.canSetPriority = true, .canSetIoPriority = true});
    inner->setIoPriorityReadResult(Platform::IoPriority{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 0});
    const auto broker = std::make_shared<Mocks::FakeBroker>(/*connected=*/false);
    ProcessDetailsPanel panel(Platform::withPrivilegedBroker(std::move(inner), broker));
    Core::ActiveTabChangedEvent shown("ProcessDetails");
    panel.onEvent(shown);
    panel.setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(1.0, 10.0)};
    for (int frame = 0; frame < 5; ++frame)
    {
        panel.updateWithSamples(samples, 1.0F);
        static_cast<void>(renderAndCapture(panel));
    }
    EXPECT_TRUE(panel.actionCapabilities().canAuthorizeInteractively);
    EXPECT_FALSE(panel.actionCapabilities().canSetRealtimeIoPriority);

    broker->setConnected(true); // "Authenticate and retry" started the helper, on a worker
    for (int frame = 0; frame < 5; ++frame)
    {
        panel.updateWithSamples(samples, 1.0F);
        static_cast<void>(renderAndCapture(panel));
    }
    EXPECT_TRUE(panel.actionCapabilities().canSetRealtimeIoPriority);
    EXPECT_EQ(broker->connectedCallCount(), 0) << "no frame looked at the helper's channel";
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

TEST_F(ProcessDetailsPanelRenderTest, ARequestedGpuTabIsSelected)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0), sampleAt(1.0, 11.0)};
    panel->updateWithSamples(samples, 1.0F);
    panel->requestTab(SelectOverride::DetailsTab::Gpu);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    // The GPU tab: this process reported no GPU figures, so its empty state, not the Overview.
    EXPECT_TRUE(text.contains("GPU")) << text;
    EXPECT_FALSE(text.contains("Identity")) << text;
    const std::vector<std::string> tabs = detailsTabLabels();
    EXPECT_TRUE(std::ranges::any_of(tabs, [](const std::string& label) { return label.contains("GPU"); }));
}

TEST_F(ProcessDetailsPanelRenderTest, ReselectingTheShownProcessKeepsItsHistoryAndAReusedPidDoesNot)
{
    const auto panel = activePanel();
    panel->setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0), sampleAt(1.0, 11.0)};
    panel->updateWithSamples(samples, 1.0F);
    ASSERT_NE(panel->displayedSnapshot(), nullptr);
    const std::uint64_t version = panel->lastSampleVersion();
    ASSERT_GT(version, 0U);

    // The same process again (a click on its row): nothing is reset.
    panel->setSelectedPid(PID, START_TICKS);
    EXPECT_NE(panel->displayedSnapshot(), nullptr);
    EXPECT_EQ(panel->lastSampleVersion(), version);

    // The same PID with another start time is another process: a new selection.
    panel->setSelectedPid(PID, START_TICKS + 1);
    EXPECT_EQ(panel->displayedSnapshot(), nullptr);
    EXPECT_EQ(panel->lastSampleVersion(), 0U);
}

TEST_F(ProcessDetailsPanelRenderTest, ThePanelWithoutTheOptionalReadersStillRenders)
{
    // The two-reader constructor: actions and environment, no connections, modules or security reader.
    ProcessDetailsPanel panel(std::make_unique<TestMocks::MockProcessActions>(),
                              std::make_unique<TestMocks::MockProcessEnvironmentReader>());
    Core::ActiveTabChangedEvent shown("ProcessDetails");
    panel.onEvent(shown);
    panel.setSelectedPid(PID, START_TICKS);
    const std::vector samples{sampleAt(0.0, 10.0)};
    panel.updateWithSamples(samples, 1.0F);
    static_cast<void>(renderAndCapture(panel));
    const std::string text = renderAndCapture(panel);
    EXPECT_TRUE(text.contains("Identity")) << text;
    EXPECT_TRUE(text.contains("Environment")) << text;
}

TEST_F(ProcessDetailsPanelRenderTest, AProcessThatLeavesTheListIsReportedExited)
{
    const auto panel = activePanel();
    EXPECT_FALSE(panel->showsRunningProcess());
    panel->setSelectedPid(PID, START_TICKS);
    EXPECT_FALSE(panel->showsRunningProcess()); // selected, not yet sampled
    const std::vector first{sampleAt(0.0, 10.0)};
    panel->updateWithSamples(first, 1.0F);
    EXPECT_TRUE(panel->showsRunningProcess());
    EXPECT_FALSE(panel->selectedProcessExited());
    static_cast<void>(renderAndCapture(*panel));

    const std::vector gone{emptySampleAt(1.0)};
    panel->updateWithSamples(gone, 1.0F);
    // What the UI training driver reads to replace the process, and not count the pane as Details (#2343).
    EXPECT_FALSE(panel->showsRunningProcess());
    EXPECT_TRUE(panel->selectedProcessExited());
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Process exited")) << text;
    EXPECT_TRUE(text.contains("server (PID 4242) is no longer running")) << text;

    // Another selection clears it.
    panel->setSelectedPid(PID + 1, START_TICKS);
    EXPECT_FALSE(panel->selectedProcessExited());
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

// --- The Actions block's dialogs when the process exits or the selection changes (#2117) ----------

/// The topmost open modal's name, or empty when none is open.
[[nodiscard]] std::string topModalTitle()
{
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    return modal != nullptr ? std::string(modal->Name) : std::string{};
}

/// Whether ImGui has any popup open at all: what KeyboardInput::anyPopupOpen() asks before it lets a
/// shortcut through. A modal left on the stack without being submitted turns them all off (#2117).
[[nodiscard]] bool anyPopupOpen()
{
    return ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

/// A panel with mock actions that can kill and dump, shown as the active tab, with "server" selected
/// and sampled once.
class ProcessDetailsDialogsTest : public ProcessDetailsPanelRenderTest
{
  protected:
    void SetUp() override
    {
        ProcessDetailsPanelRenderTest::SetUp();
        auto actions = std::make_unique<TestMocks::MockProcessActions>();
        Platform::ProcessActionCapabilities caps;
        caps.canTerminate = true;
        caps.canKill = true;
        caps.canCreateDump = true;
        actions->setCapabilities(caps);
        m_Mock = actions.get();
        m_Panel = std::make_unique<ProcessDetailsPanel>(std::move(actions));
        Core::ActiveTabChangedEvent shown("ProcessDetails");
        m_Panel->onEvent(shown);
        m_Panel->setSelectedPid(PID, START_TICKS);
        const std::vector samples{sampleAt(1.0, 10.0)};
        m_Panel->updateWithSamples(samples, 1.0F);
        static_cast<void>(renderAndCapture(*m_Panel));
    }

    void TearDown() override
    {
        // Before the ImGui context goes: the panel's dump view cancels a dump in flight and waits for it.
        m_Panel.reset();
        ProcessDetailsPanelRenderTest::TearDown();
    }

    /// "server" leaves the process list; returns the text of the frame drawn after.
    [[nodiscard]] std::string exitProcess()
    {
        const std::vector gone{emptySampleAt(2.0)};
        m_Panel->updateWithSamples(gone, 1.0F);
        return renderAndCapture(*m_Panel);
    }

    /// The dump view's confirm for "server", asked for as its button does, drawn until it is open.
    void openDumpConfirm()
    {
        dumpView().request({.pid = PID, .startTimeTicks = START_TICKS}, "server", std::filesystem::path("dumps"), m_Mock);
        static_cast<void>(dumpView().takeConfirmCheck(m_Mock, true)); // its folder, checked on the worker (#3155)
        static_cast<void>(renderAndCapture(*m_Panel));
        static_cast<void>(renderAndCapture(*m_Panel));
        ASSERT_TRUE(topModalTitle().starts_with("Create a dump file of server (PID 4242)?")) << topModalTitle();
    }

    /// Presses the open dump confirm's own button and draws until the dump is running.
    void startDump()
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(DumpDetail::CONFIRM_LABEL, 0, modal->ID));
        static_cast<void>(renderAndCapture(*m_Panel));
        static_cast<void>(renderAndCapture(*m_Panel));
        ASSERT_TRUE(dumpView().busy());
    }

    /// Presses and releases Escape over two frames.
    void pressEscape()
    {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        static_cast<void>(renderAndCapture(*m_Panel));
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        static_cast<void>(renderAndCapture(*m_Panel));
    }

    [[nodiscard]] ProcessDumpView& dumpView()
    {
        return m_Panel->actionsViewForTesting().dumpView();
    }

    TestMocks::MockProcessActions* m_Mock = nullptr; // owned by m_Panel
    std::unique_ptr<ProcessDetailsPanel> m_Panel;
};

TEST_F(ProcessDetailsDialogsTest, AKillConfirmClosesWhenItsProcessExits)
{
    m_Panel->requestKillSelected();
    static_cast<void>(renderAndCapture(*m_Panel));
    static_cast<void>(renderAndCapture(*m_Panel));
    ASSERT_TRUE(topModalTitle().starts_with("Kill")) << topModalTitle();

    const std::string text = exitProcess();
    EXPECT_TRUE(text.contains("Process exited")) << text;
    static_cast<void>(renderAndCapture(*m_Panel));
    EXPECT_EQ(topModalTitle(), "");
    EXPECT_FALSE(anyPopupOpen()); // nothing stale left to turn the shortcuts off
    EXPECT_FALSE(m_Panel->actionsViewForTesting().confirmPending());
    EXPECT_EQ(m_Mock->killCount(), 0);
}

TEST_F(ProcessDetailsDialogsTest, ADumpConfirmClosesWhenItsProcessExits)
{
    openDumpConfirm();

    static_cast<void>(exitProcess());
    static_cast<void>(renderAndCapture(*m_Panel));
    EXPECT_EQ(topModalTitle(), "");
    EXPECT_FALSE(anyPopupOpen());
    EXPECT_FALSE(dumpView().active());
    EXPECT_EQ(m_Mock->createDumpCount(), 0);
}

TEST_F(ProcessDetailsDialogsTest, ADumpConfirmClosesWhenAnotherProcessIsSelected)
{
    openDumpConfirm();

    // Another process, not sampled yet: the pane says it is not found, and the dialogs still run.
    m_Panel->setSelectedPid(PID + 1, START_TICKS + 1);
    static_cast<void>(renderAndCapture(*m_Panel));
    static_cast<void>(renderAndCapture(*m_Panel));
    EXPECT_EQ(topModalTitle(), "");
    EXPECT_FALSE(anyPopupOpen());
    EXPECT_FALSE(dumpView().active());
    EXPECT_EQ(m_Mock->createDumpCount(), 0);
}

TEST_F(ProcessDetailsDialogsTest, ADumpInFlightKeepsItsProgressAndShowsItsResultAfterItsProcessExits)
{
    m_Mock->holdDump();
    m_Mock->setDumpResult(std::unexpected(std::string("The process exited while its dump was being written")));
    openDumpConfirm();
    startDump();

    const std::string text = exitProcess();
    EXPECT_TRUE(text.contains("Process exited")) << text;
    static_cast<void>(renderAndCapture(*m_Panel));
    EXPECT_TRUE(topModalTitle().starts_with("Creating dump file")) << topModalTitle(); // its Cancel still there
    EXPECT_TRUE(dumpView().busy());

    // The dump ends: its result is taken in and shown without another selection.
    m_Mock->releaseDump();
    for (int frame = 0; frame < 400 && !dumpView().result().has_value(); ++frame)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        static_cast<void>(renderAndCapture(*m_Panel));
    }
    ASSERT_TRUE(dumpView().result().has_value());
    static_cast<void>(renderAndCapture(*m_Panel));
    // The progress dialog, now showing the result: ImGui keeps the title it was opened with as the
    // window's name, so the result's own title and text are checked on the view.
    EXPECT_TRUE(topModalTitle().ends_with("###ProcessDump")) << topModalTitle();
    const DumpDetail::DumpMessage message = dumpView().result().value_or(DumpDetail::DumpMessage{});
    EXPECT_EQ(message.title, "Couldn't create a dump file");
    EXPECT_TRUE(message.text.contains("The process exited")) << message.text;

    // Escape closes it, and nothing is left open.
    pressEscape();
    static_cast<void>(renderAndCapture(*m_Panel));
    EXPECT_EQ(topModalTitle(), "");
    EXPECT_FALSE(anyPopupOpen());
    EXPECT_FALSE(dumpView().active());
    EXPECT_EQ(m_Mock->createDumpCount(), 1);
}

} // namespace
} // namespace App
