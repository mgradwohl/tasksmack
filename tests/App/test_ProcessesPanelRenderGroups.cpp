/// @file test_ProcessesPanelRenderGroups.cpp
/// @brief The Processes panel run headless (#1546): the sampler thread and the platform it builds, rows of exited processes, grouping by
/// container, user and session, the sessions feed, and Find handles. Split from test_ProcessesPanelRender.cpp by feature (#3850); the
/// fixture is in ProcessesPanelRenderFixture.h.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ExitingProcessRows.h"
#include "App/Panels/FindHandlesDialog.h"
#include "App/Panels/NiceStep.h"
#include "App/Panels/PriorityChangesInFlight.h"
#include "App/Panels/ProcessActionsView.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDetailsPanel_ActionHelpers.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessGrouping.h"
#include "App/Panels/ProcessPins.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessSignatureText.h"
#include "App/Panels/ProcessTreeIndent.h"
#include "App/Panels/ProcessWaitChainDialog.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/Panels/StuckProcesses.h"
#include "App/Panels/UserGrouping.h"
#include "App/Panels/UserSessionsFeed.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "App/UdpCountingStatus.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/GPUModel.h"
#include "Domain/HandleSearch.h"
#include "Domain/PriorityConfig.h"
#include "Domain/ProcessModel.h"
#include "Domain/ProcessSnapshot.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Mocks/FakeClipboard.h"
#include "Mocks/FakeHttpsClient.h"
#include "Mocks/FakeUserSessions.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/CpuAffinity.h"
#include "Platform/GPUTypes.h"
#include "Platform/IHttpsClient.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessOpenFiles.h"
#include "Platform/IServiceProbe.h"
#include "Platform/IUserSessions.h"
#include "Platform/ImageSignature.h"
#include "Platform/ProcessTypes.h"
#include "Platform/SchedPolicy.h"
#include "Platform/WaitChain.h"
#include "ProcessesPanelRenderFixture.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

// #2731: onAttach() builds no probe and takes no read on the thread that calls it. The platform factory
// runs on the sampler thread, once, and the model is seeded there; the attaching thread finds the model
// in place (the shell wires it) but no rows, no capabilities and no actions until the seed is adopted.
// The factory waits on a latch until the attach-time state has been checked: a fast seed could otherwise
// land before onAttach() adopts the publication and make the check race.
TEST_F(ProcessesPanelRenderTest, OnAttachLeavesThePlatformFactoryToTheSamplerThread)
{
    const auto gate = std::make_shared<std::latch>(1);
    const auto factoryCalls = std::make_shared<std::atomic<int>>(0);
    const auto factoryThread = std::make_shared<std::atomic<std::thread::id>>();
    const auto panel = std::make_unique<ProcessesPanel>(
        [gate, factoryCalls, factoryThread]
        {
            ++*factoryCalls;
            factoryThread->store(std::this_thread::get_id());
            gate->wait();
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            Platform::ProcessCapabilities caps;
            caps.hasThreadCount = true; // off by default: its arrival shows the probe's capabilities came through
            probe->setCapabilities(caps);
            auto actions = std::make_unique<TestMocks::MockProcessActions>();
            actions->setCapabilities({.canTerminate = true, .canKill = true});
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(actions), .icons = nullptr};
        });
    panel->onAttach();
    EXPECT_NE(panel->processModel(), nullptr) << "the model exists from onAttach(), for the shell to wire";
    EXPECT_FALSE(panel->firstSnapshotAdopted());
    EXPECT_EQ(panel->processCount(), 0U) << "no read has happened on this thread";
    EXPECT_FALSE(panel->waitForFirstSample(std::chrono::milliseconds(20))) << "a seed arrived while the factory was held";

    gate->count_down();
    ASSERT_TRUE(panel->waitForFirstSample());
    EXPECT_EQ(factoryCalls->load(), 1);
    EXPECT_NE(factoryThread->load(), std::this_thread::get_id()) << "the factory ran on the attaching thread";
    EXPECT_TRUE(panel->firstSnapshotAdopted());
    EXPECT_EQ(panel->processCount(), 1U) << "the seed, taken on the sampler thread, was adopted";
    EXPECT_TRUE(panel->processCapabilities().hasThreadCount) << "the mock's capabilities came with the first generation";
    panel->onDetach();
}

// #2731: a frame drawn before the first publication -- the platform still being built on the sampler
// thread -- draws the toolbar and an empty table, not a crash; the rows appear once the seed arrives.
TEST_F(ProcessesPanelRenderTest, TheFirstFrameDrawsWithoutASnapshot)
{
    const auto gate = std::make_shared<std::latch>(1); // holds the factory, so no publication can arrive yet
    const auto panel = std::make_unique<ProcessesPanel>(
        [gate]
        {
            gate->wait();
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        });
    panel->onAttach();
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    const std::string before = renderAndCapture(*panel);
    EXPECT_FALSE(before.contains("server")) << "no generation yet: " << before;
    EXPECT_TRUE(before.contains("Columns")) << "the toolbar is drawn: " << before;

    gate->count_down();
    ASSERT_TRUE(panel->waitForFirstSample());
    static_cast<void>(renderAndCapture(*panel));
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("server")) << after;
    panel->onDetach();
}

// #3061: a pinned process's row comes first, marked, though the sort (CPU %, descending) puts it last;
// Unpin in its row menu undoes both and the saved pin. Pin to top pins the row the menu opened on.
TEST_F(ProcessesPanelRenderTest, APinnedProcessComesFirstAndUnpinUndoesIt)
{
    UserConfig::get().settings().pinnedProcesses = {"init"}; // the least CPU of the three
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);

    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains(ICON_FA_THUMBTACK)) << text;
    ASSERT_NE(text.find("init"), std::string::npos) << text;
    EXPECT_LT(text.find("init"), text.find("server")) << text;
    EXPECT_LT(text.find("init"), text.find("worker")) << text;

    // The top row is init's, so the menu opens on it.
    EXPECT_EQ(openARowMenu(*panel), std::optional<std::int32_t>{1});
    pressPopupItem(*panel, ICON_FA_THUMBTACK " Unpin###Pin");
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(UserConfig::get().settings().pinnedProcesses.empty());
    EXPECT_FALSE(text.contains(ICON_FA_THUMBTACK)) << text;
    EXPECT_LT(text.find("server"), text.find("init")) << text; // back where the sort puts it

    // And Pin to top pins the name of the row the menu opened on.
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::map<std::int32_t, std::string> names{{1, "init"}, {100, "server"}, {101, "worker"}};
    pressPopupItem(*panel, ICON_FA_THUMBTACK " Pin to top###Pin");
    EXPECT_EQ(UserConfig::get().settings().pinnedProcesses, (std::vector<std::string>{names.at(menuPid.value_or(1))}));
    panel->onDetach();
    UserConfig::get().settings().pinnedProcesses.clear();
}

// #3060: a process that exits stays as a dimmed row for a moment, not counted, then leaves. The probe
// drops it on demand; enumerate() runs on the sampler thread, so the PID to drop is an atomic.
class ExitingProbe : public TestMocks::MockProcessProbe
{
  public:
    void exitProcess(std::int32_t pid) noexcept
    {
        m_Gone.store(pid);
    }

    [[nodiscard]] std::vector<Platform::ProcessCounters> enumerate() override
    {
        std::vector<Platform::ProcessCounters> all = MockProcessProbe::enumerate();
        const std::int32_t gone = m_Gone.load();
        std::erase_if(all, [gone](const Platform::ProcessCounters& c) { return c.pid == gone; });
        return all;
    }

  private:
    std::atomic<std::int32_t> m_Gone{0};
};

class ProcessesPanelExitingRowsTest : public ProcessesPanelRenderTest
{
  protected:
    /// Renders frames, with onUpdate() before each, until @p done(panel, text) or a deadline; true if it was.
    template<typename Done> [[nodiscard]] static bool renderUntil(ProcessesPanel& panel, const Done& done, std::chrono::seconds limit)
    {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline)
        {
            panel.onUpdate(1.0F / 60.0F);
            if (done(panel, renderAndCapture(panel)))
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    [[nodiscard]] static std::unique_ptr<ProcessesPanel> exitingPanel(ExitingProbe*& probeOut)
    {
        auto panel = std::make_unique<ProcessesPanel>(
            [&probeOut]
            {
                auto probe = std::make_unique<ExitingProbe>();
                probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                    .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                    .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
                probeOut = probe.get();
                return ProcessesPanelPlatform{
                    .probe = std::move(probe),
                    .actions = std::make_unique<TestMocks::MockProcessActions>(),
                    .icons = nullptr,
                };
            });
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }
};

TEST_F(ProcessesPanelExitingRowsTest, AnExitedProcessStaysUncountedForAMomentThenLeaves)
{
    UserConfig::get().settings().highlightNewProcesses = true;
    ExitingProbe* probe = nullptr;
    const auto panel = exitingPanel(probe);
    renderUntilLoaded(*panel);
    ASSERT_NE(probe, nullptr);

    probe->exitProcess(101);
    std::string text;
    ASSERT_TRUE(renderUntil(
        *panel,
        [&text](const ProcessesPanel& p, const std::string& frame)
        {
            text = frame;
            return p.processCount() == 2;
        },
        std::chrono::seconds(30)))
        << "the sampler never published the generation without worker";
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;      // kept, dimmed
    EXPECT_TRUE(text.contains("2 processes")) << text; // and not counted

    EXPECT_TRUE(renderUntil(
        *panel, [](const ProcessesPanel&, const std::string& frame) { return !frame.contains("worker"); }, std::chrono::seconds(10)))
        << "the exited row never left";
    panel->onDetach();
}

// #3284: a process that exited while the table adopted nothing (the window hidden, or Ctrl held) exited at
// some point in that gap, not just now: the first generation adopted after it shows no exited rows.
TEST_F(ProcessesPanelExitingRowsTest, AProcessThatExitedDuringAGapIsntShownAsJustExited)
{
    UserConfig::get().settings().highlightNewProcesses = true;
    ExitingProbe* probe = nullptr;
    const auto panel = exitingPanel(probe);
    panel->setSamplingInterval(std::chrono::milliseconds(100));
    renderUntilLoaded(*panel);
    ASSERT_NE(probe, nullptr);

    // Hidden: the sampler keeps publishing, the panel adopts nothing, for longer than the gap it follows on from.
    probe->exitProcess(101);
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(ExitingProcessRows::GAP_MIN_SECONDS * 1000.0) + 500));

    std::string text;
    ASSERT_TRUE(renderUntil(
        *panel,
        [&text](const ProcessesPanel& p, const std::string& frame)
        {
            text = frame;
            return p.processCount() == 2;
        },
        std::chrono::seconds(30)))
        << "the sampler never published the generation without worker";
    text = renderAndCapture(*panel);
    EXPECT_FALSE(text.contains("worker")) << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelExitingRowsTest, WithTheSettingOffAnExitedProcessLeavesAtOnce)
{
    UserConfig::get().settings().highlightNewProcesses = false;
    ExitingProbe* probe = nullptr;
    const auto panel = exitingPanel(probe);
    renderUntilLoaded(*panel);
    ASSERT_NE(probe, nullptr);

    probe->exitProcess(101);
    ASSERT_TRUE(
        renderUntil(*panel, [](const ProcessesPanel& p, const std::string&) { return p.processCount() == 2; }, std::chrono::seconds(30)));
    const std::string text = renderAndCapture(*panel);
    EXPECT_FALSE(text.contains("worker")) << text;
    panel->onDetach();
}

// #3065: the toolbar's Run new task opens its dialog, and Run starts the command through the platform's
// actions on the row action worker, remembers it, and says so in the toolbar.
TEST_F(ProcessesPanelRenderTest, RunNewTaskStartsTheCommandAndRemembersIt)
{
    UserConfig::get().settings().recentCommands = {"htop"};
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canRunProgram = true, .canRunProgramAsAdministrator = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < 1; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(actions, nullptr);

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ROCKET "###RunNewTask", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr) << "Run new task didn't open its dialog";
    EXPECT_TRUE(renderAndCapture(*panel).contains("Run as administrator"));

    // The last command, htop, is filled in: Run starts it.
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Run", 0, ImGui::GetTopMostPopupModal()->ID)));
    EXPECT_TRUE(updateUntilBatchResult(*panel, "Started htop"));
    EXPECT_EQ(actions->runProgramCount(), 1);
    EXPECT_EQ(actions->lastRunProgram().commandLine, "htop");
    EXPECT_FALSE(actions->lastRunProgram().asAdministrator);
    EXPECT_EQ(UserConfig::get().settings().recentCommands, (std::vector<std::string>{"htop"}));
    panel->onDetach();
    UserConfig::get().settings().recentCommands.clear();
}

// #3427: a run the user called off at the system's prompt (a dismissed UAC prompt) shows in the toolbar
// in the normal text colour: nothing started, so not the success colour, and nothing went wrong, so not
// the error colour either. A run that starts is in the success colour, which shows the check can see it.
TEST_F(ProcessesPanelRenderTest, ACancelledRunNewTaskShowsInTheNormalTextColour)
{
    UserConfig::get().settings().recentCommands = {"cmd"};
    TestMocks::MockProcessActions* actions = nullptr;
    auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canRunProgram = true, .canRunProgramAsAdministrator = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < 1; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(actions, nullptr);

    // Vertices of the toolbar's own window (the Shell window it draws in) in @p colour, in the last frame.
    const auto shellVerticesIn = [](const ImVec4& colour)
    {
        const ImU32 wanted = ImGui::ColorConvertFloat4ToU32(colour);
        const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
        return (shell == nullptr)
                 ? std::ptrdiff_t{0}
                 : std::ranges::count_if(shell->DrawList->VtxBuffer, [wanted](const ImDrawVert& v) { return v.col == wanted; });
    };
    const auto runTheLastCommand = [&panel]
    {
        const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
        ASSERT_NE(shell, nullptr);
        ImGui::ActivateItemByID(ImHashStr(ICON_FA_ROCKET "###RunNewTask", 0, shell->ID));
        for (int frame = 0; frame < 3; ++frame)
        {
            static_cast<void>(renderAndCapture(*panel));
        }
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr) << "Run new task didn't open its dialog";
        ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Run", 0, modal->ID)));
    };
    // Before any result: what the rest of the toolbar draws in those colours. The tests' built-in scheme
    // may give success and error one colour, so each is counted against its own baseline.
    const auto& scheme = UI::Theme::get().scheme();
    const std::ptrdiff_t successBefore = shellVerticesIn(scheme.textSuccess);
    const std::ptrdiff_t errorBefore = shellVerticesIn(scheme.textError);

    runTheLastCommand();
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Started cmd"));
    EXPECT_GT(shellVerticesIn(scheme.textSuccess), successBefore) << "the started run isn't in the success colour";

    actions->setRunProgramResult(Platform::ProcessActionResult::cancelledByUser("the administrator prompt was cancelled"));
    runTheLastCommand();
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Didn't start cmd: the administrator prompt was cancelled"));
    EXPECT_EQ(shellVerticesIn(scheme.textSuccess), successBefore) << "a cancelled run is shown as a success";
    EXPECT_EQ(shellVerticesIn(scheme.textError), errorBefore) << "a cancelled run is shown as an error";
    EXPECT_EQ(actions->runProgramCount(), 2);
    panel->onDetach();
    UserConfig::get().settings().recentCommands.clear();
}

// #3068: grouped by container, each container is a header row with its process count and Σ totals, its
// processes under it, the rest under Ungrouped last; clicking a header collapses it; Tree sets the group
// back to None.
TEST_F(ProcessesPanelRenderTest, GroupByContainerShowsHeadersWithTotalsAndCollapses)
{
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::Container;
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            auto web = TestMocks::makeProcessCounters(100, "nginx", 'S', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            web.container = "docker:1a2b3c4d5e6f";
            auto worker = TestMocks::makeProcessCounters(101, "nginx-worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100);
            worker.container = "docker:1a2b3c4d5e6f";
            Platform::ProcessCapabilities caps;
            caps.hasContainer = true; // Group: Container is offered only where containers are read (#3428)
            probe->setCapabilities(caps);
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(web)
                .withProcess(worker);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);

    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Group: Container")) << text;
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f (2)")) << text;
    EXPECT_TRUE(text.contains("Ungrouped (1)")) << text;
    EXPECT_TRUE(text.contains("\xCE\xA3 10.0 MiB")) << text; // the container's two RSS figures, totalled
    EXPECT_LT(text.find("docker:1a2b3c4d5e6f"), text.find("Ungrouped")) << text;
    EXPECT_LT(text.find("docker:1a2b3c4d5e6f"), text.find("nginx-worker")) << text;
    EXPECT_LT(text.find("nginx-worker"), text.find("Ungrouped")) << text;

    // Clicking the container's header row (the first row) collapses it: its processes leave the table.
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    // The process table: the one with every visible column, drawn in the Shell window or its child.
    const ImGuiTable* table = nullptr;
    for (int i = 0; i < GImGui->Tables.GetMapSize(); ++i)
    {
        if (const ImGuiTable* t = GImGui->Tables.TryGetMapData(i); t != nullptr && t->ColumnsCount >= 5)
        {
            table = t;
        }
    }
    ASSERT_NE(table, nullptr);
    const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
    const ImVec2 firstRow((table->Columns[1].MinX + table->Columns[1].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(firstRow.x, firstRow.y);
    static_cast<void>(renderAndCapture(*panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    static_cast<void>(renderAndCapture(*panel));
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f (2)")) << text;
    EXPECT_FALSE(text.contains("nginx-worker")) << text;

    // Tree view and grouping are separate views: Tree sets the group back to None.
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_SITEMAP " Tree", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_EQ(UserConfig::get().settings().processGroupBy, ProcessGrouping::GroupBy::None);
    EXPECT_TRUE(text.contains("Group: None")) << text;
    panel->onDetach(); // TearDown restores the settings
}

// #3807: group headers are formatted once per groups rebuild, not every frame: frames with the same data reuse the
// header names (and the totals and statuses beside them), and a collapse, which changes the caret, rebuilds them.
TEST_F(ProcessesPanelRenderTest, GroupHeadersAreFormattedOncePerRebuildNotEveryFrame)
{
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::Container;
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            auto web = TestMocks::makeProcessCounters(100, "nginx", 'S', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            web.container = "docker:1a2b3c4d5e6f";
            Platform::ProcessCapabilities caps;
            caps.hasContainer = true;
            probe->setCapabilities(caps);
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0)).withProcess(web);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("docker:1a2b3c4d5e6f (1)")) << text;
    const std::size_t builds = panel->groupHeaderTextBuildsForTesting();
    for (int frame = 0; frame < 10; ++frame)
    {
        text = renderAndCapture(*panel);
    }
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f (1)")) << text;
    EXPECT_TRUE(text.contains("Ungrouped (1)")) << text;
    // Two headers drawn ten times would be twenty builds if formatted every frame; a sample that lands meanwhile
    // rebuilds the groups once (two builds), which is allowed twice over.
    EXPECT_LE(panel->groupHeaderTextBuildsForTesting() - builds, 4U) << "ten frames, the same headers";
    panel->onDetach();
}

// #3428: where the probe reads no containers and no app units (Windows), a saved Group: Container stands
// as None, the Group control doesn't offer Container or App, and the saved choice is kept for a system that
// has them; choosing None there replaces it.
TEST_F(ProcessesPanelRenderTest, TheGroupControlOffersOnlyTheModesThisSystemHasDataFor)
{
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::Container;
    const auto panel = attachedPanel(); // the mock probe's default capabilities: no containers, no app units
    renderUntilLoaded(*panel);
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Group: None")) << text;
    EXPECT_FALSE(text.contains("Ungrouped")) << text;
    EXPECT_EQ(panel->groupBy(), ProcessGrouping::GroupBy::None);
    EXPECT_EQ(UserConfig::get().settings().processGroupBy, ProcessGrouping::GroupBy::Container); // kept

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##GroupBy", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(topPopup(), nullptr);
    // The open list's items: the words drawn between the Group control and the Columns button.
    text = renderAndCapture(*panel);
    const std::size_t listStart = text.find("Group: None");
    const std::size_t listEnd = text.find("Columns");
    ASSERT_NE(listStart, std::string::npos) << text;
    ASSERT_NE(listEnd, std::string::npos) << text;
    ASSERT_LT(listStart, listEnd) << text;
    const std::size_t itemsStart = listStart + std::string_view("Group: None").size();
    std::istringstream list(text.substr(itemsStart, listEnd - itemsStart));
    std::vector<std::string> offered;
    for (std::string word; list >> word;)
    {
        offered.push_back(word);
    }
    const auto offers = [&offered](std::string_view label)
    {
        return std::ranges::find(offered, label) != offered.end();
    };
    EXPECT_TRUE(offers("None")) << text;
    EXPECT_TRUE(offers("Unit")) << text;
    EXPECT_TRUE(offers("User")) << text;
    EXPECT_FALSE(offers("Container")) << text;
    EXPECT_FALSE(offers("App")) << text;
    pressPopupItem(*panel, "None");
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(UserConfig::get().settings().processGroupBy, ProcessGrouping::GroupBy::None);
    panel->onDetach(); // TearDown restores the settings
}

// --- Signed-in sessions in Group: User (#3377) ---------------------------------------------------

/// A sign-in session of @p user.
[[nodiscard]] Platform::UserSession signIn(std::string id, std::string user, std::string state = "active")
{
    Platform::UserSession s;
    s.id = std::move(id);
    s.user = std::move(user);
    s.state = std::move(state);
    s.sessionClass = "user";
    return s;
}

/// An attached, shown panel reading init (1) as root, server (100) as alice and worker (101) as bob, with the
/// signed-in sessions from @p feed, already started, which outlives the panel.
[[nodiscard]] std::unique_ptr<ProcessesPanel> userSessionsPanel(UserSessionsFeed& feed)
{
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            const auto owned = [](Platform::ProcessCounters counters, std::string user)
            {
                counters.user = std::move(user);
                return counters;
            };
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(owned(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0), "root"))
                .withProcess(owned(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1), "alice"))
                .withProcess(owned(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100), "bob"));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        [](Core::Event& /*event*/) {});
    panel->setUserSessions(&feed);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    return panel;
}

class ProcessesPanelUserGroupsTest : public ProcessesPanelRenderTest
{
  protected:
    /// Draws frames until one shows @p needle (the sessions are read on the feed's thread); that frame's text.
    [[nodiscard]] static std::string renderUntil(ProcessesPanel& panel, std::string_view needle)
    {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        std::string text = renderAndCapture(panel);
        while (!text.contains(needle) && std::chrono::steady_clock::now() < giveUp)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            text = renderAndCapture(panel);
        }
        return text;
    }

    /// The middle of the table's Name column at data row @p row (0: the first under the column headers).
    [[nodiscard]] static ImVec2 rowPoint(int row)
    {
        const ImGuiTable* table = processTable();
        if (table == nullptr)
        {
            return {-1.0F, -1.0F};
        }
        const float rowHeight = ImGui::GetTextLineHeight() + (ImGui::GetStyle().CellPadding.y * 2.0F);
        const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
        return {(name.MinX + name.MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * (1.5F + static_cast<float>(row)))};
    }

    /// Right-clicks data row @p row, hovering it first, as a pointer would; the frame after the release.
    static std::string rightClickRow(ProcessesPanel& panel, int row)
    {
        const ImVec2 point = rowPoint(row);
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(point.x, point.y);
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        static_cast<void>(renderAndCapture(panel)); // the popup's first frame only sizes it
        std::string text = renderAndCapture(panel);
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return text;
    }

    /// Draws frames until the session actions' line says @p result.
    [[nodiscard]] static bool renderUntilResult(ProcessesPanel& panel, std::string_view result)
    {
        return renderUntil(panel, result).contains(result);
    }
};

// #3377: grouped by User, each signed-in account's sessions are under its header, read only while the table
// shows them: grouped any other way, or with the tab hidden, nothing is read. The signed-in accounts come
// first (decision 1), one with no process too; a session row is no process, so a click on it selects nothing.
TEST_F(ProcessesPanelUserGroupsTest, GroupByUserShowsEachSignedInAccountsSessionsReadOnlyWhileShown)
{
    Platform::UserSession desktop = signIn("2", "alice");
    desktop.seat = "seat0";
    desktop.tty = "tty2";
    desktop.type = "wayland";
    desktop.current = true;
    auto fake = std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{desktop, signIn("9", "carol", "online")});
    UserSessionsFeed feed([fake] { return fake; });
    feed.start();
    feed.setInterval(std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::None;
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    EXPECT_FALSE(feed.demanded());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(fake->m_Reads.load(), 0) << "read while not grouped by User";

    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User; // the next frame groups by it
    const std::string text = renderUntil(*panel, "carol (0)");
    EXPECT_TRUE(feed.demanded());
    EXPECT_TRUE(text.contains(ICON_FA_USER "  alice  (you) (1)")) << text;
    EXPECT_TRUE(text.contains(ICON_FA_DESKTOP "  seat0 \xC2\xB7 tty2 \xC2\xB7 Wayland  (this session)")) << text;
    EXPECT_TRUE(text.contains("Active")) << text;
    EXPECT_TRUE(text.contains(ICON_FA_USER "  carol (0)")) << "signed in, with no process: " << text;
    EXPECT_TRUE(text.contains("Signed in")) << text;
    EXPECT_LT(text.find("alice  (you)"), text.find("carol (0)")) << text;
    EXPECT_LT(text.find("carol (0)"), text.find("root (1)")) << "signed in first: " << text;
    EXPECT_LT(text.find("carol (0)"), text.find("bob (1)")) << text;
    EXPECT_LT(text.find("seat0"), text.find("server")) << "the sessions under the header, before its processes: " << text;

    // Rows: alice, her session, server, carol, her session, then root's and bob's. A click on a session selects nothing.
    const ImVec2 session = rowPoint(1);
    clickAtPoint(*panel, session.x, session.y);
    EXPECT_EQ(panel->selectedPid(), -1) << "a session row isn't a process";
    // The keyboard moves to processes only: the first is alice's server, past her session.
    pressKey(*panel, ImGuiKey_DownArrow);
    EXPECT_EQ(panel->selectedPid(), 100);

    // Another tab shown: the table reads nothing.
    Core::ActiveTabChangedEvent hidden("Services");
    panel->onEvent(hidden);
    panel->onUpdate(1.0F / 60.0F);
    EXPECT_FALSE(feed.demanded());
    panel->onDetach();
    feed.stop();
}

// #3377 (Linux): a session's menu has Lock and Sign out...; Sign out asks first, naming
// the session. Refused for want of an administrator, the toolbar's line offers Authenticate and retry. An
// account with one session offers the same menu on its header: Lock runs at once.
TEST_F(ProcessesPanelUserGroupsTest, ASessionsMenuSignsOutAfterAskingAndTheHeaderLocksItsOneSession)
{
    auto fake =
        std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{signIn("2", "alice"), signIn("5", "carol")});
    UserSessionsFeed feed([fake] { return fake; });
    feed.start();
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderUntil(*panel, "carol (0)").contains("carol (0)"));
    ASSERT_NE(feed.actions(), nullptr);

    // Rows: alice, her session, server, carol, her session. carol's session: Sign out...
    const std::string menu = rightClickRow(*panel, 4);
    ASSERT_NE(topPopup(), nullptr) << "the session menu didn't open";
    EXPECT_TRUE(menu.contains("Lock")) << menu;
    EXPECT_TRUE(menu.contains("Sign out...")) << menu;
    EXPECT_FALSE(menu.contains("Disconnect...")) << menu;
    pressPopupItem(*panel, ICON_FA_RIGHT_FROM_BRACKET "  Sign out...");
    ASSERT_TRUE(feed.actions()->pending().has_value());
    EXPECT_EQ(feed.actions()->pending()->id, "5");
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("Sign out carol")) << "the confirm names the session";
    pressModalFilledButton(*panel, "Sign out");
    ASSERT_TRUE(renderUntilResult(*panel, "Couldn't sign out carol's session: Requires administrator"));
    EXPECT_TRUE(renderAndCapture(*panel).contains(App::AuthorizeWording::RETRY_TEXT)) << "offered in the toolbar";
    feed.actions()->retryAuthorized();
    ASSERT_TRUE(renderUntilResult(*panel, "Signed out carol's session"));

    // alice's header: her one session's menu; Lock runs without asking, and the sessions are read again.
    static_cast<void>(rightClickRow(*panel, 0));
    ASSERT_NE(topPopup(), nullptr) << "the header's menu didn't open";
    const int readsBefore = fake->m_Reads.load();
    pressPopupItem(*panel, ICON_FA_LOCK "  Lock");
    EXPECT_FALSE(feed.actions()->pending().has_value()) << "Lock doesn't ask";
    ASSERT_TRUE(renderUntilResult(*panel, "Locked alice's session"));
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"signout 5", "signout 5 interactive", "lock 2"}));
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (fake->m_Reads.load() == readsBefore && std::chrono::steady_clock::now() < giveUp)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GT(fake->m_Reads.load(), readsBefore) << "the done action asks for a fresh read";
    panel->onDetach();
    feed.stop();
}

// #3803: an action started under Group: User that finishes after the grouping changed still runs out its result line
// and asks for a fresh read, so Group: User doesn't come back to a stale line.
TEST_F(ProcessesPanelUserGroupsTest, AnActionThatFinishesAfterTheGroupingChangesLeavesNoStaleLine)
{
    auto fake =
        std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{signIn("2", "alice"), signIn("5", "carol")});
    UserSessionsFeed feed([fake] { return fake; });
    feed.start();
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    // carol has no process, so her header is there only once the sessions are read: alice's header has its menu then.
    ASSERT_TRUE(renderUntil(*panel, "carol (0)").contains("carol (0)"));
    ASSERT_NE(feed.actions(), nullptr);

    // alice's header: her one session's Lock, which runs at once; then the table is grouped another way.
    static_cast<void>(rightClickRow(*panel, 0));
    ASSERT_NE(topPopup(), nullptr) << "the header's menu didn't open";
    pressPopupItem(*panel, ICON_FA_LOCK "  Lock");
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::None;
    const int readsBefore = fake->m_Reads.load();

    // Long frames, so the line's few seconds pass in a few of them, while the worker finishes.
    ImGui::GetIO().DeltaTime = 1.0F;
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((feed.actions()->busy() || !feed.actions()->result().empty()) && std::chrono::steady_clock::now() < giveUp)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ImGui::GetIO().DeltaTime = 1.0F / 60.0F;
    EXPECT_EQ(fake->calls(), (std::vector<std::string>{"lock 2"}));
    EXPECT_FALSE(feed.actions()->busy());
    EXPECT_TRUE(feed.actions()->result().empty()) << "the line ran out while not grouped by User";

    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    const std::string text = renderUntil(*panel, "carol (0)");
    EXPECT_FALSE(text.contains("Locked alice's session")) << "a stale line: " << text;
    EXPECT_GE(fake->m_Reads.load(), readsBefore);
    panel->onDetach();
    feed.stop();
}

// #3377 (Windows): the menu offers Disconnect... where Linux has Lock, greyed for a session already
// disconnected; on another, it opens its own confirm.
TEST_F(ProcessesPanelUserGroupsTest, WindowsSessionMenusOfferDisconnectInLocksPlace)
{
    auto fake = std::make_shared<TestMocks::FakeUserSessions>(
        std::vector<Platform::UserSession>{signIn("1", "alice"), signIn("4", "carol", "disconnected")}, true, true);
    UserSessionsFeed feed([fake] { return fake; });
    feed.start();
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderUntil(*panel, "Disconnected").contains("carol (0)"));

    // carol's disconnected session: Disconnect... is greyed, so pressing it does nothing.
    const std::string menu = rightClickRow(*panel, 4);
    ASSERT_NE(topPopup(), nullptr) << "the session menu didn't open";
    EXPECT_TRUE(menu.contains("Disconnect...")) << menu;
    EXPECT_TRUE(menu.contains("Sign out...")) << menu;
    EXPECT_FALSE(menu.contains("Lock")) << menu;
    pressPopupItem(*panel, ICON_FA_LINK_SLASH "  Disconnect...");
    EXPECT_FALSE(feed.actions()->pending().has_value()) << "an already disconnected session";
    closePopups(*panel);

    // alice's session: Disconnect... asks.
    static_cast<void>(rightClickRow(*panel, 1));
    ASSERT_NE(topPopup(), nullptr);
    pressPopupItem(*panel, ICON_FA_LINK_SLASH "  Disconnect...");
    ASSERT_TRUE(feed.actions()->pending().has_value());
    EXPECT_EQ(feed.actions()->pending()->id, "1");
    EXPECT_EQ(feed.actions()->pendingAction(), Platform::SessionAction::Disconnect);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("Disconnect alice")) << "the Disconnect confirm";
    feed.actions()->cancel();
    EXPECT_TRUE(fake->calls().empty());
    panel->onDetach();
    feed.stop();
}

// #3377 decision 2: while the filter is on, an account with sign-in sessions and no process it matches still
// shows, its header and its sessions, with a line saying none matches; an account not signed in doesn't.
TEST_F(ProcessesPanelUserGroupsTest, AnAccountWithSessionsAndNoMatchingProcessesStillShowsWhileFiltered)
{
    auto fake =
        std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{signIn("2", "alice"), signIn("9", "carol")});
    UserSessionsFeed feed([fake] { return fake; });
    feed.start();
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderUntil(*panel, "carol (0)").contains("carol (0)"));
    EXPECT_FALSE(renderAndCapture(*panel).contains(UserGrouping::NO_MATCHING_PROCESSES)) << "no filter, nothing to say";

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##search", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    for (const char* character : {"w", "o", "r", "k"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("alice (0)")) << "signed in: still shown, " << text;
    EXPECT_TRUE(text.contains("carol (0)")) << text;
    EXPECT_TRUE(text.contains("bob (1)")) << text;
    EXPECT_FALSE(text.contains("root")) << "not signed in, no match: " << text;
    EXPECT_TRUE(text.contains(UserGrouping::NO_MATCHING_PROCESSES)) << text;
    panel->onDetach();
    feed.stop();
}

// #3377 (PR 5): the Processes panel owns the feed's life: it starts the feed at attach
// (nothing read until the table is grouped by User), hands it each Settings interval, and stops it at detach,
// joining its sampler thread and letting the sessions go.
TEST_F(ProcessesPanelUserGroupsTest, ThePanelStartsTheFeedAtAttachHandsItTheIntervalAndStopsItAtDetach)
{
    const auto fake = std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{signIn("2", "alice")});
    UserSessionsFeed feed([fake] { return fake; });
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::None;
    ASSERT_EQ(feed.model(), nullptr);
    const auto panel = userSessionsPanel(feed); // attached here, the feed not started by the test
    ASSERT_NE(feed.model(), nullptr) << "onAttach() didn't start the feed";
    EXPECT_FALSE(feed.demanded()) << "not grouped by User";

    Core::RefreshRateChangedEvent faster(Domain::Sampling::REFRESH_INTERVAL_MIN_MS);
    panel->onEvent(faster);
    EXPECT_EQ(feed.interval(), std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS));

    renderUntilLoaded(*panel);
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    EXPECT_TRUE(renderUntil(*panel, ICON_FA_USER "  alice").contains(ICON_FA_USER "  alice")) << "the session never read";
    EXPECT_TRUE(feed.demanded());

    panel->onDetach();
    EXPECT_EQ(feed.model(), nullptr) << "onDetach() didn't stop the feed";
    EXPECT_FALSE(feed.demanded());
    const int readsStopped = fake->m_Reads.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_MIN_MS * 2));
    EXPECT_EQ(fake->m_Reads.load(), readsStopped) << "read after the panel detached";
}

// #3377: a failed read says so in one muted line above the table, which groups the processes as ever; where the
// platform lists no sessions (a synthetic run), there are no session rows and no message, and nothing is read.
TEST_F(ProcessesPanelUserGroupsTest, FailedAndUnsupportedSessionReads)
{
    auto failing = std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{});
    failing->m_Failure = "systemd-logind isn't running";
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::User;
    {
        UserSessionsFeed feed([failing] { return failing; });
        feed.start();
        const auto panel = userSessionsPanel(feed);
        renderUntilLoaded(*panel);
        const std::string text = renderUntil(*panel, "couldn't be read");
        EXPECT_TRUE(text.contains("Signed-in sessions couldn't be read: systemd-logind isn't running")) << text;
        EXPECT_TRUE(text.contains("root (1)")) << text;
        panel->onDetach();
        feed.stop();
    }

    auto unsupported = std::make_shared<TestMocks::FakeUserSessions>(std::vector<Platform::UserSession>{signIn("2", "alice")}, false);
    UserSessionsFeed feed([unsupported] { return unsupported; });
    feed.start();
    const auto panel = userSessionsPanel(feed);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_FALSE(feed.demanded());
    EXPECT_EQ(unsupported->m_Reads.load(), 0);
    EXPECT_TRUE(text.contains("alice (1)")) << text;
    EXPECT_FALSE(text.contains("couldn't be read")) << text;
    EXPECT_FALSE(text.contains(ICON_FA_USER)) << "no sessions, no signed-in accounts: " << text;
    panel->onDetach();
    feed.stop();
}

// #3457: appMembership(), what Process Details' App row says, is the App group Group: App puts the process in,
// worked out from the model's whole latest generation: a terminal's shell is in the terminal's group, named for
// it and marked as there through its parent; the terminal is in its own; a process in no app, one the generation
// doesn't list, one started at another time and no selection are in none. It reads the model, not the table's
// render cache, so it answers while the Processes tab has never been shown.
TEST_F(ProcessesPanelRenderTest, AppMembershipFollowsTheTablesAppGroupFromTheWholeGeneration)
{
    static constexpr std::string_view FAMILY = "Microsoft.WindowsTerminal_8wekyb3d8bbwe";
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            auto terminal = TestMocks::makeProcessCounters(20, "WindowsTerminal.exe", 'S', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            terminal.packageFamily = FAMILY;
            terminal.appName = "Windows Terminal";
            Platform::ProcessCapabilities caps;
            caps.hasAppIds = true; // Group: App is offered where app IDs are read (#3457)
            probe->setCapabilities(caps);
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(terminal)
                .withProcess(TestMocks::makeProcessCounters(21, "pwsh.exe", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 20))
                .withProcess(TestMocks::makeProcessCounters(30, "notepad.exe", 'S', 30, 10, 1200, std::uint64_t{2} << 20U, 1));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        });
    // onAttach() alone, not attachAndSeed(): waitForFirstSample() and onUpdate() adopt the seed into the table's
    // render cache. The seed's publication is waited for through the model (findSnapshot()), so the cache stays
    // empty and appMembership() can only answer from the model's generation.
    panel->onAttach();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (panel->findSnapshot(30) == nullptr && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_NE(panel->findSnapshot(30), nullptr) << "the sampler thread did not publish the seed in time";
    ASSERT_EQ(panel->adoptedProcessCount(), 0U) << "the render cache adopted a generation";

    const ProcessGrouping::AppMembership shell = panel->appMembership(21, 0);
    EXPECT_EQ(shell.key, FAMILY);
    EXPECT_EQ(shell.label, "Windows Terminal");
    EXPECT_TRUE(shell.inherited);
    EXPECT_TRUE(shell.tooltip.contains("through a parent process")) << shell.tooltip;

    const std::shared_ptr<const Domain::ProcessSnapshot> terminalSnapshot = panel->findSnapshot(20);
    ASSERT_NE(terminalSnapshot, nullptr);
    const std::uint64_t terminalTicks = terminalSnapshot != nullptr ? terminalSnapshot->startTimeTicks : 0;
    const ProcessGrouping::AppMembership terminal = panel->appMembership(20, terminalTicks);
    EXPECT_EQ(terminal.key, FAMILY);
    EXPECT_EQ(terminal.label, "Windows Terminal");
    EXPECT_FALSE(terminal.inherited);
    EXPECT_EQ(terminal.tooltip, std::format("Package family: {}", FAMILY));

    EXPECT_EQ(panel->appMembership(20, terminalTicks + 1), ProcessGrouping::AppMembership{})
        << "a process started at another time is another process";
    EXPECT_EQ(panel->appMembership(30, 0), ProcessGrouping::AppMembership{}) << "notepad is in no app";
    EXPECT_EQ(panel->appMembership(999, 0), ProcessGrouping::AppMembership{}) << "the generation doesn't list it";
    EXPECT_EQ(panel->appMembership(-1, 0), ProcessGrouping::AppMembership{}) << "nothing is selected";
    EXPECT_EQ(panel->appMembership(21, 0), shell) << "asked again, the same answer";
    EXPECT_EQ(panel->adoptedProcessCount(), 0U) << "appMembership() adopted into the render cache";
    panel->onDetach();
}

// #3067: the toolbar's Find handles opens its dialog; a query typed there searches every process's open
// files on the dialog's worker and lists each process holding a match, with the count in the status line.
// #3335: a search whose worker returned without its done publication reads as stopped, with its last counts, not
// as still searching; one still running reads as searching.
TEST(FindHandlesStatusTest, AWorkerThatReturnedWithoutItsDonePublicationReadsAsStopped)
{
    Domain::HandleSearch::SearchPublication running = Domain::HandleSearch::startFor("x", 10, 3);
    running.searched = 4;
    EXPECT_TRUE(App::findHandlesStatus(running, false).starts_with("Searching... 4 of 10 processes"));
    const std::string stopped = App::findHandlesStatus(running, true);
    EXPECT_TRUE(stopped.starts_with("4 processes searched")) << stopped;
    EXPECT_TRUE(stopped.ends_with("The search stopped before it finished")) << stopped;
    running.done = true;
    running.failure = "The search stopped: injected";
    EXPECT_TRUE(App::findHandlesStatus(running, true).ends_with("The search stopped: injected"));
}

// #3335: a reader that throws on the Find handles worker doesn't terminate TaskSmack: the search ends, the
// status line says why, and Find is offered again.
TEST_F(ProcessesPanelRenderTest, AFindHandlesReaderThatThrowsEndsTheSearchAndSaysWhy)
{
    class ThrowingOpenFiles final : public Platform::IProcessOpenFilesReader
    {
      public:
        [[nodiscard]] bool hasOpenFiles() const override
        {
            return true;
        }
        [[nodiscard]] Platform::OpenFilesReadResult readOpenFiles(const Platform::ProcessTarget& /*target*/) override
        {
            throw std::runtime_error("injected reader failure");
        }
    };
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(42, "soffice", 'S', 30, 10, 200, std::uint64_t{4} << 20U, 1));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
                .openFiles = std::make_unique<ThrowingOpenFiles>(),
            };
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);

    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F);
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_F, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddKeyEvent(ImGuiKey_F, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "Ctrl+Shift+F didn't open Find handles";
    ImGui::GetIO().AddInputCharactersUTF8("x");
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    std::string text;
    for (int frame = 0; frame < LOAD_FRAMES; ++frame)
    {
        text = renderAndCapture(*panel);
        // Both in one frame: the worker can finish between the query row (Stop) and the status line.
        if (text.contains("The search stopped") && !text.contains("Stop"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(text.contains("The search stopped: injected reader failure")) << text;
    EXPECT_FALSE(text.contains("Searching...")) << "the search ended: " << text;
    EXPECT_FALSE(text.contains("Stop")) << "Find is offered again, not Stop: " << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, FindHandlesListsTheProcessesHoldingAFile)
{
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(42, "soffice", 'S', 30, 10, 200, std::uint64_t{4} << 20U, 1));
            auto files = std::make_unique<TestMocks::MockProcessOpenFilesReader>();
            Platform::OpenFilesReadResult held;
            held.status = Platform::OpenFilesReadStatus::Ok;
            for (const char* path : {"/home/me/Report.ods", "/home/me/notes.txt"})
            {
                Platform::OpenFile open;
                open.kind = Platform::OpenFileKind::File;
                open.path = path;
                held.files.push_back(std::move(open));
            }
            files->setResult(std::move(held));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
                .openFiles = std::move(files),
            };
        });
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    // Ctrl+Shift+F over the table opens it, as the toolbar's magnifier button does.
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // hovered: the table's keys are armed
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(ImGuiKey_F, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddKeyEvent(ImGuiKey_F, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "Ctrl+Shift+F didn't open Find handles";
    ASSERT_NE(shell, nullptr);
    EXPECT_TRUE(renderAndCapture(*panel).contains("Type a name and press Find."));

    // The query box has the keyboard when the dialog opens: type, then Enter searches.
    for (const char* character : {"r", "e", "p", "o", "r", "t"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    std::string text;
    for (int frame = 0; frame < LOAD_FRAMES; ++frame)
    {
        text = renderAndCapture(*panel);
        if (text.contains("2 processes searched"))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(text.contains("2 processes searched \xC2\xB7 2 found")) << text;
    EXPECT_TRUE(text.contains("/home/me/Report.ods")) << text;
    EXPECT_TRUE(text.contains("soffice")) << text;
    EXPECT_FALSE(text.contains("notes.txt")) << text;
    panel->onDetach();
}

} // namespace
} // namespace App
