/// @file test_ProcessesPanelRender.cpp
/// @brief The Processes panel run headless (#1546): capabilities and counters, an inactive panel, the row menu's actions (Copy PID, Analyze
/// wait chain, VirusTotal, scheduling policy), action results, F9 and the meters. Split from test_ProcessesPanelRender.cpp by feature
/// (#3850); the fixture is in ProcessesPanelRenderFixture.h.

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

#ifndef _WIN32
// #3366, #3580: once its platform is adopted with no UDP collector (a Standard session: only TaskSmack started as root
// loads the counter), the panel tells Settings what counting needs, and keeps the setting on offer.
TEST_F(ProcessesPanelRenderTest, AnAdoptedPlatformWithNoUdpCollectorSaysCountingNeedsAdministrator)
{
    const auto panel = attachedPanel();
    const App::UdpCountingStatus status = App::udpCountingStatus();
    EXPECT_FALSE(status.unsupported) << "the setting stays on offer for the next Administrator session";
    EXPECT_FALSE(status.delivering);
    EXPECT_NE(status.problem.find("TaskSmack (Administrator)"), std::string::npos) << status.problem;
}
#endif

TEST_F(ProcessesPanelRenderTest, TheTableListsTheProbesProcesses)
{
    const auto panel = attachedPanel();
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_TRUE(text.contains("100")) << text;
    panel->onDetach();
}

// #1891: every column's cell renderer draws its cells -- the hidden-by-default ones too, which no other
// test turned on. A few at a time beside PID and Name, so none is scrolled out of the table's width.
TEST_F(ProcessesPanelRenderTest, EveryColumnDrawsItsHeaderAndCells)
{
    constexpr std::size_t GROUP = 6;
    const auto columns = allProcessColumns();
    for (std::size_t first = 0; first < columns.size(); first += GROUP)
    {
        // As the config file would have it: only this group (and PID and Name) shown.
        ProcessColumnSettings& settings = UserConfig::get().settings().processColumns;
        std::vector<ProcessColumn> group;
        for (std::size_t i = 0; i < columns.size(); ++i)
        {
            const bool inGroup = i >= first && i < first + GROUP;
            settings.setVisible(columns[i], inGroup);
            if (inGroup)
            {
                group.push_back(columns[i]);
            }
        }
        const auto panel = attachedPanel();
        static_cast<void>(renderAndCapture(*panel));
        static_cast<void>(renderAndCapture(*panel));
        const std::string text = renderAndCapture(*panel);
        EXPECT_TRUE(text.contains("server")) << "the rows are still drawn";
        for (const ProcessColumn col : group)
        {
            EXPECT_TRUE(text.contains(getColumnInfo(col).name)) << getColumnInfo(col).configKey << " in:\n" << text;
        }
        panel->onDetach();
    }
}

TEST_F(ProcessesPanelRenderTest, TheTreeViewKeepsEveryProcess)
{
    const auto panel = attachedPanel();
    const bool wasTree = panel->treeViewEnabled();
    panel->toggleTreeView();
    EXPECT_NE(panel->treeViewEnabled(), wasTree);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    // The child follows its parent in either view: tree order, or the list's default sort.
    EXPECT_LT(text.find("server"), text.find("worker")) << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, AnInactivePanelDrawsNothing)
{
    const auto panel = attachedPanel();
    Core::ActiveTabChangedEvent away("SystemOverview");
    panel->onEvent(away);
    EXPECT_TRUE(renderAndCapture(*panel).empty());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheKeyboardSelectsARowAndAnnouncesIt)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);
    ASSERT_TRUE(events.selected.empty());

    pressKey(*panel, ImGuiKey_DownArrow);
    ASSERT_EQ(events.selected.size(), 1U);
    EXPECT_EQ(events.selected.back(), panel->selectedPid()); // the event names the process now selected
    EXPECT_EQ(panel->selectionCount(), 1U);
    EXPECT_EQ(events.showDetails, 0); // selecting doesn't open Details

    // The next row is another process, announced in turn.
    pressKey(*panel, ImGuiKey_DownArrow);
    ASSERT_EQ(events.selected.size(), 2U);
    EXPECT_NE(events.selected[1], events.selected[0]);
    EXPECT_EQ(events.selected[1], panel->selectedPid());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, EndSelectsTheLastRow)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);
    pressKey(*panel, ImGuiKey_Home);
    ASSERT_FALSE(events.selected.empty());
    const std::int32_t first = events.selected.back();
    pressKey(*panel, ImGuiKey_End);
    ASSERT_GE(events.selected.size(), 2U);
    EXPECT_NE(events.selected.back(), first);
    EXPECT_EQ(events.selected.back(), panel->selectedPid());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, ARightClickOpensTheRowMenuForThatProcess)
{
    SelectionSink events;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions(events.sink(), actions);
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);

    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::int32_t pid = menuPid.value_or(-1);
    // The press selected the row it opened on, and the menu names that same process (#1365).
    EXPECT_EQ(panel->selectedPid(), pid);
    ASSERT_FALSE(events.selected.empty());
    EXPECT_EQ(events.selected.back(), pid);

    pressPopupItem(*panel, ICON_FA_CIRCLE_INFO " Details");
    EXPECT_EQ(events.showDetails, 1);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, CopyPidPutsThePidOnTheClipboard)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, ICON_FA_COPY " Copy PID");
    ASSERT_NE(ImGui::GetClipboardText(), nullptr);
    EXPECT_EQ(std::string(ImGui::GetClipboardText()), std::to_string(menuPid.value_or(-1)));
    panel->onDetach();
}

// #1891: the row menu's Copy Name and Copy Command Line copy that process's, to the panel's clipboard.
TEST_F(ProcessesPanelRenderTest, CopyNameAndCopyCommandLinePutThemOnTheClipboard)
{
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                counters.command = "/usr/bin/" + counters.name + " --flag";
                probe->withProcess(counters);
            }
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::map<std::int32_t, std::string> names{{1, "init"}, {100, "server"}, {101, "worker"}};

    std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::string name = names.at(menuPid.value_or(1));
    pressPopupItem(*panel, ICON_FA_COPY " Copy Name");
    ASSERT_NE(ImGui::GetClipboardText(), nullptr);
    EXPECT_EQ(std::string(ImGui::GetClipboardText()), name);

    if (topPopup() != nullptr) // The item closed the menu, or a dialog it opened is still closing
    {
        closePopups(*panel);
    }
    menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, ICON_FA_COPY " Copy Command Line");
    ASSERT_NE(ImGui::GetClipboardText(), nullptr);
    EXPECT_EQ(std::string(ImGui::GetClipboardText()), "/usr/bin/" + names.at(menuPid.value_or(1)) + " --flag");
    panel->onDetach();
}

// #1891: Suspend and Resume from the row menu are confirmed, then sent for the process the menu was for.
TEST_F(ProcessesPanelRenderTest, SuspendAndResumeFromTheRowMenuAreConfirmedThenSent)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);

    std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, std::format("{} Suspend...###Suspend", ICON_FA_PAUSE));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_EQ(actions->stopCount(), 0);  // asked, not done
    pressModalButton(*panel, "Suspend"); // Not destructive: a plain button, not the danger fill
    panel->waitForRowActionForTesting(); // it runs on the row action worker (#2305)
    EXPECT_EQ(actions->stopCount(), 1);
    EXPECT_EQ(actions->lastStopPid(), menuPid.value_or(-1));

    if (topPopup() != nullptr) // The item closed the menu, or a dialog it opened is still closing
    {
        closePopups(*panel);
    }
    menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, std::format("{} Resume...###Resume", ICON_FA_PLAY));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    pressModalButton(*panel, "Resume");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->resumeCount(), 1);
    EXPECT_EQ(actions->lastResumePid(), menuPid.value_or(-1));
    panel->onDetach();
}

// #1891: where the platform can write a dump (#807), the row menu offers "Create dump file..." for that
// one process: it opens the dump's confirm, naming it; Cancel writes nothing.
TEST_F(ProcessesPanelRenderTest, CreateDumpFromTheRowMenuAsksFirst)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canCreateDump = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    // The panel's dump view reads this session's elevation (#4141), and elevated its default is the admin-only
    // %ProgramData%\TaskSmack\Dumps, which the confirm would make (#4188). Pinned to %TEMP%, as the Process Details
    // harnesses do, so an elevated run neither behaves differently nor touches a real system folder (#4269).
    panel->dumpViewForTesting().setNoDefaultFolder(false);
    panel->dumpViewForTesting().setDefaultFolder(&DumpDetail::tempDumpDirectory);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::map<std::int32_t, std::string> names{{1, "init"}, {100, "server"}};

    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, DumpDetail::CREATE_DUMP_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    const std::int32_t pid = menuPid.value_or(1);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with(DumpDetail::confirmTitle(names.at(pid), pid))) << modal->Name;

    pressModalButton(*panel, "Cancel");
    EXPECT_EQ(actions->createDumpCount(), 0);
    panel->onDetach();
}

// #3566's review, #3889: where the actions can set the Realtime I/O class (an Administrator session, #3580), the
// batch I/O priority dialog's slider offers Realtime as Process Details' does. It takes that from the copy read when
// the platform was adopted, and opening it asks the actions nothing: since #3580 nothing changes after that.
TEST_F(ProcessesPanelRenderTest, TheBatchIoPriorityDialogOffersRealtimeWhereTheActionsCanSetIt)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (const auto& counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canSetIoPriority = true, .canSetRealtimeIoPriority = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const int asked = actions->actionCapabilitiesCount();
    ASSERT_GE(asked, 1) << "read when the platform was built";

    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, "###SetIoPriority");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(panel->batchIoPriorityDialogForTesting().isPending());
    EXPECT_EQ(panel->batchIoPriorityDialogForTesting().count(), 3U);
    EXPECT_TRUE(panel->batchIoPriorityDialogForTesting().realtimeSettable());
    EXPECT_EQ(actions->actionCapabilitiesCount(), asked) << "opening the dialog asked the actions nothing";
    panel->onDetach();
}

// #3071: the row menu's Efficiency mode, for a process not in it, asks to confirm in Task Manager's words,
// then turns it on for the process the menu was for.
TEST_F(ProcessesPanelRenderTest, EfficiencyModeFromTheRowMenuIsConfirmedThenTurnedOn)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = efficiencyModePanel("", actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Turn on Efficiency mode for")) << modal->Name;
    EXPECT_EQ(actions->efficiencyModeCount(), 0); // asked, not done

    pressModalButton(*panel, "Turn on Efficiency mode"); // not destructive: a plain button
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->efficiencyModeCount(), 1);
    EXPECT_TRUE(actions->lastEfficiencyMode());
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1));
    EXPECT_TRUE(renderAndCapture(*panel).contains(std::format("Efficiency mode on for PID {}", menuPid.value_or(-1))));
    panel->onDetach();
}

// #3071: ticked for a process whose Status says it is in Efficiency mode; pressing it turns it off at once,
// with no confirm. On a selection of several it is greyed out: one process at a time, as Task Manager.
TEST_F(ProcessesPanelRenderTest, EfficiencyModeTickedInTheRowMenuIsTurnedOffAtOnceAndOneProcessAtATime)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = efficiencyModePanel(std::string(Platform::EFFICIENCY_MODE_STATUS), actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr) << "turning it off isn't confirmed";
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->efficiencyModeCount(), 1);
    EXPECT_FALSE(actions->lastEfficiencyMode());
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1));

    if (topPopup() != nullptr)
    {
        closePopups(*panel);
    }
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->efficiencyModeCount(), 1) << "greyed out for a selection: nothing is sent";
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    panel->onDetach();
}

// #3429: where the platform can set Realtime (Windows), the row menu's Set Realtime priority... asks first, with
// the warning and the danger fill, then sets Realtime's nice for the process the menu was for. A refusal for want
// of rights says what Windows did, as the platform words it.
TEST_F(ProcessesPanelRenderTest, SetRealtimePriorityFromTheRowMenuWarnsThenSetsRealtime)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canSetPriority = true, .canSetRealtimePriority = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);

    std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, App::Detail::SET_REALTIME_PRIORITY_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Set Realtime priority for")) << modal->Name;
    EXPECT_TRUE(text.contains("can make the desktop unresponsive")) << text;
    EXPECT_EQ(actions->setPriorityCount(), 0); // asked, not done

    pressModalFilledButton(*panel, "Set Realtime priority"); // the danger fill
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setPriorityCount(), 1);
    EXPECT_EQ(actions->lastSetPriorityNice(), App::Detail::REALTIME_PRIORITY_NICE);
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1));
    EXPECT_TRUE(renderAndCapture(*panel).contains(std::format("Priority of PID {} set to Realtime", menuPid.value_or(-1))));

    // Windows quietly set High: the toolbar says so.
    actions->setPriorityResult(Platform::ProcessActionResult::denied("Windows set High: Realtime needs administrator rights."));
    if (topPopup() != nullptr)
    {
        closePopups(*panel);
    }
    menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, App::Detail::SET_REALTIME_PRIORITY_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    pressModalFilledButton(*panel, "Set Realtime priority");
    panel->waitForRowActionForTesting();
    EXPECT_TRUE(renderAndCapture(*panel).contains("Windows set High: Realtime needs administrator rights.")) << "the toolbar";
    panel->onDetach();
}

// #3084: the row menu's Analyze wait chain... opens the wait chain dialog for the process the menu was for,
// which reads its chains on its worker, says so meanwhile, then shows them.
TEST_F(ProcessesPanelRenderTest, AnalyzeWaitChainFromTheRowMenuReadsThatProcesssChains)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (const auto& counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canAnalyzeWaitChain = true});
            Platform::WaitChainReadResult read;
            read.status = Platform::WaitChainReadStatus::Ok;
            read.threadCount = 1;
            Platform::ThreadWaitChain chain;
            chain.tid = 4321;
            Platform::WaitChainNode self;
            self.type = Platform::WaitObjectType::Thread;
            self.status = Platform::WaitNodeStatus::Blocked;
            Platform::WaitChainNode lock;
            lock.type = Platform::WaitObjectType::SendMessageCall;
            lock.status = Platform::WaitNodeStatus::Blocked;
            chain.nodes = {self, lock};
            read.threads.push_back(chain);
            mock->setWaitChainResult(read);
            mock->holdWaitChains();
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, WaitChainDetail::ANALYZE_WAIT_CHAIN_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    const std::string pending = renderAndCapture(*panel);
    EXPECT_TRUE(pending.contains(WaitChainDetail::READING_TEXT)) << pending;
    EXPECT_TRUE(panel->waitChainDialogForTesting().isOpen());
    EXPECT_EQ(panel->waitChainDialogForTesting().target().pid, menuPid.value_or(-1));

    actions->releaseWaitChains();
    panel->waitChainDialogForTesting().update(true);
    const std::string read = renderAndCapture(*panel);
    EXPECT_TRUE(read.contains("Thread 4321: waiting on SendMessage to a window")) << read;
    EXPECT_EQ(actions->waitChainCount(), 1);
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, KillFromTheRowMenuIsConfirmedThenSentForThatProcess)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, std::format("{} Kill...###Kill", ICON_FA_SKULL));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Kill")) << modal->Name;
    EXPECT_EQ(actions->killCount(), 0); // asked, not done

    // The dialog's danger-filled Kill button (UI::Widgets::filledButton(): "##filled" under its label).
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Kill", 0, modal->ID)));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->killCount(), 1);
    EXPECT_EQ(actions->lastKillPid(), menuPid.value_or(-1));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    panel->onDetach();
}

// #3221: Open file location runs on the row action worker, for the row the menu was opened on, through
// the platform's actions (here the mock's: no file manager is started).
TEST_F(ProcessesPanelRenderTest, OpenFileLocationFromTheRowMenuRunsForThatProcess)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = panelWithCapabilities({.canOpenFileLocation = true}, false, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, ICON_FA_FOLDER_OPEN " Open file location###OpenFileLocation");
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->openFileLocationCount(), 1);
    // Its result line counts down with the frames after it.
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    panel->onDetach();
}

// #3094: Look up on VirusTotal is in the row menu with a key and a platform that can hash the image, and runs on the
// row action worker for that row: the image's hash, then one request for it.
TEST_F(ProcessesPanelRenderTest, LookUpOnVirusTotalFromTheRowMenuHashesAndAsksOnce)
{
    const std::string sha = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    UserConfig::get().settings().virusTotalApiKey = std::string(64, 'a');
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = panelWithCapabilities({.canHashImage = true}, false, actions);
    ASSERT_NE(actions, nullptr);
    actions->setImageSha256Result(sha);
    const auto client = std::make_shared<ProcessesPanelRenderTestDetail::FakeLookupClient>(Platform::HttpsResponse{
        .body = R"({"data":{"attributes":{"last_analysis_stats":{"malicious":0,"suspicious":0,"undetected":70,"harmless":0}}}})",
        .status = 200,
    });
    panel->setLookupClient(client);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, ICON_FA_SHIELD_HALVED " Look up on VirusTotal###LookUpVirusTotal");
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->imageSha256Count(), 1);
    EXPECT_EQ(client->requestCount(), 1U);
    EXPECT_EQ(client->lastPath(), "/api/v3/files/" + sha);
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    panel->onDetach();
}

// #3094: without a key there is no item to press, and nothing is hashed or sent.
TEST_F(ProcessesPanelRenderTest, WithoutAKeyThereIsNoVirusTotalLookup)
{
    UserConfig::get().settings().virusTotalApiKey.clear();
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = panelWithCapabilities({.canHashImage = true}, false, actions);
    ASSERT_NE(actions, nullptr);
    const auto client = std::make_shared<ProcessesPanelRenderTestDetail::FakeLookupClient>(Platform::HttpsResponse{});
    panel->setLookupClient(client);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, ICON_FA_SHIELD_HALVED " Look up on VirusTotal###LookUpVirusTotal");
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->imageSha256Count(), 0);
    EXPECT_EQ(client->requestCount(), 0U);
    panel->onDetach();
}

// #3221: a selection requested by PID (a demo script's, #2827) is applied once its process is listed.
TEST_F(ProcessesPanelRenderTest, ARequestedSelectionSelectsItsProcessOnceListed)
{
    const auto panel = attachedPanel([](Core::Event& /*event*/) {}); // the selection's event, unread here
    panel->requestSelection(SelectOverride::Target{.pid = 100, .name = {}});
    updateUntilLoaded(*panel);
    for (int frame = 0; frame < 5 && panel->selectedPid() != 100; ++frame)
    {
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
    }
    EXPECT_EQ(panel->selectedPid(), 100);
    panel->onDetach();
}

// #3221: Terminate from the row menu is confirmed first, and End process tree on a process with
// children asks once for it and everything it started.
TEST_F(ProcessesPanelRenderTest, TerminateAndEndProcessTreeFromTheRowMenuAreConfirmedFirst)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = panelWithCapabilities({.canTerminate = true, .canKill = true}, true, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);

    ASSERT_TRUE(openARowMenu(*panel).has_value());
    const char* terminate = Detail::actionLabel(Detail::ProcessAction::Terminate);
    pressPopupItem(*panel, std::format("{} {}...###{}", ICON_FA_XMARK, terminate, terminate));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* confirm = ImGui::GetTopMostPopupModal();
    ASSERT_NE(confirm, nullptr) << "Terminate is confirmed";
    EXPECT_TRUE(std::string_view(confirm->Name).starts_with(terminate)) << confirm->Name;
    EXPECT_EQ(actions->terminateCount(), 0);
    pressModalButton(*panel, "Cancel");

    // The tree view's first row is init, the parent of the others.
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_EQ(menuPid, 1);
    pressPopupItem(*panel, ICON_FA_SITEMAP " End process tree###EndProcessTree");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* tree = ImGui::GetTopMostPopupModal();
    ASSERT_NE(tree, nullptr) << "the tree is confirmed";
    EXPECT_EQ(actions->terminateCount(), 0) << "asked, not done";
    panel->onDetach();
}

// #3221: a panel that was never attached has no snapshot to find, no first sample to wait for, and no
// reduced privileges to report.
TEST_F(ProcessesPanelRenderTest, APanelThatWasNeverAttachedHasNothingToReport)
{
    const ProcessesPanel panel(
        []
        { return ProcessesPanelPlatform{.probe = std::make_unique<TestMocks::MockProcessProbe>(), .actions = nullptr, .icons = nullptr}; },
        [](Core::Event&) {});
    EXPECT_EQ(panel.findSnapshot(1), nullptr);
    EXPECT_FALSE(panel.hasReducedPrivileges());
    ProcessesPanel unattached(
        []
        { return ProcessesPanelPlatform{.probe = std::make_unique<TestMocks::MockProcessProbe>(), .actions = nullptr, .icons = nullptr}; },
        [](Core::Event&) {});
    EXPECT_FALSE(unattached.waitForFirstSample(std::chrono::milliseconds(1)));
}

// #1891: an action result too long for the toolbar is cut off with an ellipsis, and hovering it shows
// the whole message in a tooltip.
TEST_F(ProcessesPanelRenderTest, AClippedActionResultShowsInFullOnHover)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    constexpr std::string_view TAIL = "END-OF-THE-LONG-MESSAGE";
    actions->setKillResult(Platform::ProcessActionResult::error(std::string(400, 'x') + " " + std::string(TAIL)));
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, std::format("{} Kill...###Kill", ICON_FA_SKULL));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr("Kill", 0, modal->ID)));
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting(); // the kill runs on the row action worker (#2305)
    // The cut-off text is logged once as drawn; the tooltip, when shown, logs it a second time.
    const auto copies = [&panel, TAIL]
    {
        const std::string text = renderAndCapture(*panel);
        int n = 0;
        for (std::size_t at = text.find(TAIL); at != std::string::npos; at = text.find(TAIL, at + 1))
        {
            ++n;
        }
        return n;
    };
    ASSERT_EQ(copies(), 1) << "drawn once, cut off";

    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    // Along the toolbar row at the top of the panel, right to left (the status sits at its right end).
    const float lineHeight = ImGui::GetFrameHeight();
    bool shown = false;
    for (int row = 1; row <= 4 && !shown; ++row)
    {
        const float y = lineHeight * 0.5F * static_cast<float>(row);
        for (int step = 0; static_cast<float>(step) * 8.0F < PANEL_RENDER_DISPLAY_WIDTH && !shown; ++step)
        {
            ImGui::GetIO().AddMousePosEvent(PANEL_RENDER_DISPLAY_WIDTH - 2.0F - (static_cast<float>(step) * 8.0F), y);
            static_cast<void>(renderAndCapture(*panel));
            shown = copies() >= 2;
        }
    }
    EXPECT_TRUE(shown) << "the cut-off result's tooltip";
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, SetAffinityFromTheRowMenuAppliesTheDialogsCpus)
{
    // Every process may run on CPU 0 only, and the actions can set an affinity (#1887) on a PC with
    // two CPUs online, a layout read once on the sampler thread with the actions (#3182).
    TestMocks::MockAffinityLayoutActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                counters.cpuAffinity = Platform::CpuAffinity::fromMask(0b1U);
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockAffinityLayoutActions>();
            mock->setCapabilities({.canSetAffinity = true});
            Platform::CpuAffinityLayout layout;
            layout.online = Platform::CpuAffinity::fromMask(0b11U);
            mock->setAffinityLayout(std::move(layout));
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, ICON_FA_MICROCHIP " Set affinity...###SetAffinity");
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr); // the dialog, with the process's own CPU ticked
    EXPECT_TRUE(text.contains("1 of 2 CPUs")) << "boxes for the layout's CPUs: " << text;
    EXPECT_EQ(actions->affinityLayoutCount(), 1) << "read once, with the actions";
    EXPECT_NE(actions->affinityLayoutThread(), std::this_thread::get_id()) << "on the sampler thread, never the UI thread";
    EXPECT_EQ(actions->setAffinityCount(), 0);

    pressModalFilledButton(*panel, "Apply");
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setAffinityCount(), 1);
    EXPECT_EQ(actions->lastAffinity(), Platform::CpuAffinity::fromMask(0b1U));
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1)); // the process the menu was for
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    panel->onDetach();
}

/// A panel over three processes under @p policy whose actions can set a scheduling policy (#3086) and ask for
/// authorization; @p actions is left pointing at them.
[[nodiscard]] std::unique_ptr<ProcessesPanel> schedPolicyPanel(Platform::SchedPolicy policy, TestMocks::MockProcessActions*& actions)
{
    return std::make_unique<ProcessesPanel>(
        [&actions, policy]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                counters.schedPolicy = policy;
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canSetSchedPolicy = true, .canAuthorizeInteractively = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
}

// #3086: the row menu's Set scheduling policy... opens the dialog on the process's own policy; Apply with Idle
// chosen sets it on the process the menu was for, and the toolbar says so.
TEST_F(ProcessesPanelRenderTest, SetSchedulingPolicyFromTheRowMenuAppliesTheDialogsChoice)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = schedPolicyPanel(Platform::SchedPolicy::Batch, actions);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    pressPopupItem(*panel, ICON_FA_CLOCK " Set scheduling policy...###SetSchedPolicy");
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_TRUE(text.contains("Runs only when nothing else wants the CPU")) << text;
    EXPECT_EQ(actions->setSchedPolicyCount(), 0) << "nothing is set before Apply";

    pressModalButton(*panel, "Idle"); // its radio button
    pressModalFilledButton(*panel, "Apply");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setSchedPolicyCount(), 1);
    EXPECT_EQ(actions->lastSchedPolicy(), Platform::SchedPolicy::Idle);
    EXPECT_EQ(actions->lastRealtimeThreads(), Platform::RealtimeThreads::Keep) << "opened on Batch: no warning, none demoted";
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1)); // the process the menu was for
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_NE(renderAndCapture(*panel).find("set to Idle"), std::string::npos) << "the result line";
    panel->onDetach();
}

// #3086: leaving Idle without CAP_SYS_NICE is refused for want of permission. No retry is offered, even where the
// actions could ask for authorization (#3886): the policy is Linux's, and no helper redoes it (#3580).
TEST_F(ProcessesPanelRenderTest, ARefusedSchedulingPolicyOffersNoRetry)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = schedPolicyPanel(Platform::SchedPolicy::Idle, actions);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    actions->setSchedPolicyResult(Platform::ProcessActionResult::denied("Permission denied"));
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, ICON_FA_CLOCK " Set scheduling policy...###SetSchedPolicy");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalButton(*panel, "Normal");
    pressModalFilledButton(*panel, "Apply");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setSchedPolicyCount(), 1);
    EXPECT_TRUE(actions->actionCapabilities().canAuthorizeInteractively) << "a retry would be possible";
    const std::string line = renderAndCapture(*panel);
    EXPECT_TRUE(line.contains("Permission denied")) << line;
    EXPECT_FALSE(panel->offersAuthenticateAndRetry());
    EXPECT_EQ(actions->runAuthorizedCount(), 0);
    panel->onDetach();
}

// #3219: real time from the row menu: RR shows the warning, Apply asks to confirm naming the process, and Set sets RR
// at the slider's priority (50 for a process not on real time); refused for want of permission, no retry is
// offered (#3886).
TEST_F(ProcessesPanelRenderTest, RoundRobinFromTheRowMenuIsSetAfterTheConfirm)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = schedPolicyPanel(Platform::SchedPolicy::Normal, actions);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    actions->setSchedPolicyResult(Platform::ProcessActionResult::denied("Permission denied"));
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, ICON_FA_CLOCK " Set scheduling policy...###SetSchedPolicy");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalButton(*panel, "RR");
    EXPECT_TRUE(renderAndCapture(*panel).contains("starve everything else"));
    pressModalFilledButton(*panel, "Apply");
    const std::string confirm = renderAndCapture(*panel);
    EXPECT_TRUE(confirm.contains(std::format("(PID {}) to RR 50?", menuPid.value_or(-1)))) << confirm;
    EXPECT_EQ(actions->setSchedPolicyCount(), 0) << "nothing is set before the confirm";
    pressModalFilledButton(*panel, "Set");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setSchedPolicyCount(), 1);
    EXPECT_EQ(actions->lastSchedPolicy(), Platform::SchedPolicy::RoundRobin);
    EXPECT_EQ(actions->lastRtPriority(), 50);
    EXPECT_EQ(actions->lastTarget().pid, menuPid.value_or(-1));
    EXPECT_FALSE(panel->offersAuthenticateAndRetry());
    EXPECT_EQ(actions->runAuthorizedCount(), 0);
    panel->onDetach();
}

// #3086: no Set scheduling policy... where the platform can't set one (Windows), nor for a selection of several.
TEST_F(ProcessesPanelRenderTest, TheRowMenuOffersSetSchedulingPolicyOnlyWhereThePlatformCan)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    actions->setCapabilities({.canTerminate = true, .canKill = true});
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    EXPECT_FALSE(renderAndCapture(*panel).contains("Set scheduling policy")) << "the platform can't set one";
    panel->onDetach();
}

/// Actions whose kill waits until the test opens the gate: a password prompt or a helper's round trip
/// as the panel sees it (#2305).
class GatedKillActions : public TestMocks::MockProcessActions
{
  public:
    explicit GatedKillActions(std::atomic<bool>& gate) : m_Gate(&gate)
    {}

    [[nodiscard]] Platform::ProcessActionResult kill(const Platform::ProcessTarget& target) override
    {
        m_Gate->wait(false);
        return TestMocks::MockProcessActions::kill(target);
    }

  private:
    std::atomic<bool>* m_Gate;
};

/// Opens a gate (GatedKillActions') when it goes out of scope, so a failed assertion never leaves a worker
/// waiting on it for the panel's destructor to wait on in turn.
class GateOpener
{
  public:
    explicit GateOpener(std::atomic<bool>& gate) : m_Gate(&gate)
    {}
    GateOpener(const GateOpener&) = delete;
    GateOpener& operator=(const GateOpener&) = delete;
    GateOpener(GateOpener&&) = delete;
    GateOpener& operator=(GateOpener&&) = delete;
    ~GateOpener()
    {
        open(*m_Gate);
    }

    static void open(std::atomic<bool>& gate)
    {
        gate = true;
        gate.notify_all();
    }

  private:
    std::atomic<bool>* m_Gate;
};

// #2305: a confirmed kill runs on the row action worker. The frames go on meanwhile, the toolbar says
// "Killing PID n..." and the row menu offers no other action; its result line comes once it returns.
TEST_F(ProcessesPanelRenderTest, ARowActionRunsOffTheFrameAndShowsItsProgress)
{
    std::atomic<bool> gate{false};
    GatedKillActions* actions = nullptr;
    const auto panel = std::make_unique<ProcessesPanel>(
        [&gate, &actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1));
            auto mock = std::make_unique<GatedKillActions>(gate);
            mock->setCapabilities({.canTerminate = true, .canKill = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    const GateOpener guard(gate); // the gate opens at the end whatever happens, before the panel waits for its worker

    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, std::format("{} Kill...###Kill", ICON_FA_SKULL));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalFilledButton(*panel, "Kill");

    const std::string pending = std::format("Killing PID {}...", menuPid.value_or(-1));
    EXPECT_NE(renderAndCapture(*panel).find(pending), std::string::npos) << "the toolbar shows the kill in progress";
    EXPECT_NE(renderAndCapture(*panel).find(pending), std::string::npos) << "and the frames go on";

    GateOpener::open(gate);
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->killCount(), 1);
    EXPECT_EQ(actions->lastKillPid(), menuPid.value_or(-1));
    EXPECT_EQ(renderAndCapture(*panel).find(pending), std::string::npos) << "its result replaces the progress";
    panel->onDetach();
}

// #2308: a row action refused for want of permission, where the actions can ask for authorization,
// offers "Authenticate and retry" in the toolbar; pressing it sends the action through runAuthorized().
TEST_F(ProcessesPanelRenderTest, ARefusedRowActionOffersAuthenticateAndRetry)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    actions->setCapabilities({.canTerminate = true, .canKill = true, .canAuthorizeInteractively = true});
    actions->setKillResult(Platform::ProcessActionResult::denied("Permission denied - process belongs to another user"));
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    pressPopupItem(*panel, std::format("{} Kill...###Kill", ICON_FA_SKULL));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalFilledButton(*panel, "Kill");
    panel->waitForRowActionForTesting();
    ASSERT_TRUE(panel->offersAuthenticateAndRetry());
    EXPECT_NE(renderAndCapture(*panel).find(App::AuthorizeWording::RETRY_TEXT), std::string::npos) << "drawn in the toolbar";

    panel->retryRowActionAuthorizedForTesting();
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->runAuthorizedCount(), 1);
    EXPECT_EQ(actions->lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::Kill);
    EXPECT_EQ(actions->lastAuthorizedAction().target.pid, menuPid.value_or(-1));
    EXPECT_FALSE(panel->offersAuthenticateAndRetry());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, F9OnOneSelectedRowAsksToKillThatProcess)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 1); // a plain click selects one row
    ASSERT_EQ(heights.size(), 1U);
    ASSERT_EQ(panel->selectionCount(), 1U);
    const std::int32_t pid = heights.begin()->first;

    panel->requestKillSelected(); // F9, as ShellLayer hands it to the tab on show
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).starts_with("Kill")) << modal->Name;
    EXPECT_EQ(actions->killCount(), 0); // asked, not done

    pressModalFilledButton(*panel, "Kill");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->killCount(), 1);
    EXPECT_EQ(actions->lastKillPid(), pid); // the highlighted row's process
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheColumnsMenuShowsAHiddenColumn)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    ASSERT_FALSE(renderAndCapture(*panel).contains("Threads")); // hidden by default

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(topPopup(), nullptr);
    pressPopupItem(*panel, "Threads");
    EXPECT_NE(topPopup(), nullptr); // stays open while columns are ticked

    // Closed, the table has the column from the next frame.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("Threads"));
    panel->onDetach();
}

// #1799: the Columns menu lists NPU %, NPU Memory and NPU Engine, all off by default. Shown with no
// NPU to read -- this panel has no GPU model -- the column's cells are the unavailable dash, as the
// GPU columns' are without per-process data.
TEST_F(ProcessesPanelRenderTest, TheColumnsMenuListsTheNpuColumnsOffByDefault)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::string before = renderAndCapture(*panel);
    ASSERT_FALSE(before.contains("NPU")) << before; // no NPU column shown by default

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    const std::string menu = renderAndCapture(*panel);
    ASSERT_NE(topPopup(), nullptr);
    for (const char* item : {"NPU %", "NPU Memory", "NPU Engine"})
    {
        EXPECT_TRUE(menu.contains(item)) << item << " in\n" << menu;
    }
    for (const ProcessColumn col : {ProcessColumn::NpuPercent, ProcessColumn::NpuMemory, ProcessColumn::NpuEngine})
    {
        EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(col)) << getColumnInfo(col).configKey;
    }
    pressPopupItem(*panel, "NPU Memory");

    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    static_cast<void>(renderAndCapture(*panel));
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("NPU Mem")) << after;
    // No NPU here: each row's new cell is the unavailable dash.
    const auto dashes = [](const std::string& text)
    {
        return std::ranges::count(text, '\x94');
    };
    EXPECT_GE(dashes(after), dashes(before) + 3) << after;
    EXPECT_TRUE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::NpuMemory));
    panel->onDetach();
}

// #1870: on a machine with an NPU, NPU % is shown by default -- once the GPU model has listed the NPU,
// not before -- and NPU Memory and NPU Engine are not. A default, not a choice, so it is not saved.
TEST_F(ProcessesPanelRenderTest, NpuPercentIsShownByDefaultOnceTheNpuIsKnown)
{
    const auto gpu = gpuModel(/*withNpu=*/true);
    const auto panel = attachedPanel({}, gpu);
    renderUntilLoaded(*panel);
    const std::string before = renderAndCapture(*panel);
    EXPECT_FALSE(before.contains("NPU")) << before; // Not known yet: not shown, then withdrawn

    gpu->refresh();                              // The GPU sampler's first sample lists the NPU
    static_cast<void>(renderAndCapture(*panel)); // Queues the new default
    static_cast<void>(renderAndCapture(*panel)); // The table applies it
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("NPU %")) << after;
    EXPECT_FALSE(after.contains("NPU Mem")) << after;
    EXPECT_FALSE(after.contains("NPU Engine")) << after;
    const ProcessColumnSettings& saved = UserConfig::get().settings().processColumns;
    EXPECT_TRUE(saved.isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(saved.isChosen(ProcessColumn::NpuPercent));

    // Turned off in the Columns menu: a choice, saved as such, and the column goes.
    openColumnsMenu(*panel);
    pressPopupItem(*panel, "NPU %");
    closePopups(*panel);
    EXPECT_FALSE(renderAndCapture(*panel).contains("NPU %"));
    EXPECT_TRUE(UserConfig::get().settings().processColumns.isChosen(ProcessColumn::NpuPercent));
    EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::NpuPercent));
    panel->onDetach();
}

// #1870, #2736: the NPU listed before the table is first drawn -- the Processes tab opened after the GPU
// sampler's first sample, as on a first launch, which opens on the machine's tab. NPU % is shown, and
// it stays a default. Its new default was applied on the frame ImGui created the table, which then set
// every column from its flags and lost it; the next frame took the difference for the user's own toggle
// and saved npu_percent = false.
TEST_F(ProcessesPanelRenderTest, NpuPercentListedBeforeTheTableIsFirstDrawnIsShownAndNotChosen)
{
    const auto gpu = gpuModel(/*withNpu=*/true);
    const auto panel = attachedPanel({}, gpu);
    gpu->refresh(); // Listed before the panel's first frame
    renderUntilLoaded(*panel);
    static_cast<void>(renderAndCapture(*panel));
    const std::string after = renderAndCapture(*panel);
    EXPECT_TRUE(after.contains("NPU %")) << after;
    const ProcessColumnSettings& saved = UserConfig::get().settings().processColumns;
    EXPECT_TRUE(saved.isVisible(ProcessColumn::NpuPercent));
    EXPECT_FALSE(saved.isChosen(ProcessColumn::NpuPercent));
    panel->onDetach();
}

// #1870: a choice made in the Columns menu before the NPU is known is not overridden when it is.
TEST_F(ProcessesPanelRenderTest, AColumnsMenuChoiceMadeBeforeTheNpuIsKnownIsKept)
{
    const auto gpu = gpuModel(/*withNpu=*/true);
    const auto panel = attachedPanel({}, gpu);
    renderUntilLoaded(*panel);
    openColumnsMenu(*panel);
    pressPopupItem(*panel, "NPU %"); // On...
    pressPopupItem(*panel, "NPU %"); // ...and off again: off, as a choice
    closePopups(*panel);

    gpu->refresh();
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("NPU %"));
    EXPECT_TRUE(UserConfig::get().settings().processColumns.isChosen(ProcessColumn::NpuPercent));
    EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::NpuPercent));
    panel->onDetach();
}

// #1870: "npu_percent = false" in the config file wins over the NPU's default.
TEST_F(ProcessesPanelRenderTest, AnExplicitNpuPercentOffWinsOverTheNpu)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::NpuPercent, false);
    const auto gpu = gpuModel(/*withNpu=*/true);
    gpu->refresh();
    const auto panel = attachedPanel({}, gpu);
    renderUntilLoaded(*panel);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("NPU %"));
    EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::NpuPercent));
    panel->onDetach();
}

// #1870: without an NPU, NPU % is off by default; shown by the user -- in the Columns menu, or the
// config file next time -- it stays, and each row's cell is the unavailable dash.
TEST_F(ProcessesPanelRenderTest, NpuPercentShownWithoutAnNpuReadsAsTheDash)
{
    const auto gpu = gpuModel(/*withNpu=*/false);
    gpu->refresh();
    {
        const auto panel = attachedPanel({}, gpu);
        renderUntilLoaded(*panel);
        static_cast<void>(renderAndCapture(*panel));
        const std::string before = renderAndCapture(*panel);
        EXPECT_FALSE(before.contains("NPU")) << before;

        openColumnsMenu(*panel);
        pressPopupItem(*panel, "NPU %");
        closePopups(*panel);
        const std::string after = renderAndCapture(*panel);
        EXPECT_TRUE(after.contains("NPU %")) << after;
        const auto dashes = [](const std::string& text)
        {
            return std::ranges::count(text, '\x94');
        };
        EXPECT_GE(dashes(after), dashes(before) + 3) << after;
        EXPECT_TRUE(UserConfig::get().settings().processColumns.isChosen(ProcessColumn::NpuPercent));
        panel->onDetach();
    }

    // As the config file has it next time: still shown.
    ASSERT_TRUE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::NpuPercent));
    const auto panel = attachedPanel({}, gpu);
    renderUntilLoaded(*panel);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("NPU %"));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheMeterMenuTurnsAMeterOn)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const bool wasOn = UserConfig::get().settings().processMeters.isOn(ProcessColumn::CpuPercent);

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(topPopup(), nullptr);
    pressPopupItem(*panel, ICON_FA_CHART_BAR " Show meter"); // opens the submenu
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* submenu = topPopup();
    ASSERT_NE(submenu, nullptr);
    EXPECT_TRUE(std::string_view(submenu->Name).contains("Show meter")) << submenu->Name;
    // A row per column, a radio button per style (#1612): under the table's ID and the column's index.
    const auto pressStyle = [&panel](ProcessColumn col, const char* style)
    {
        const ImGuiWindow* menu = topPopup();
        ASSERT_NE(menu, nullptr);
        const int index = static_cast<int>(toIndex(col));
        const ImGuiID row = ImHashData(&index, sizeof(index), ImHashStr("##meter_styles", 0, menu->ID));
        ImGui::ActivateItemByID(ImHashStr(style, 0, row));
        static_cast<void>(renderAndCapture(*panel));
    };
    pressStyle(ProcessColumn::CpuPercent, wasOn ? "Off" : "Bar");
    EXPECT_NE(UserConfig::get().settings().processMeters.isOn(ProcessColumn::CpuPercent), wasOn);

    // A sparkline has the model keep that column's history, and only while one is shown.
    pressStyle(ProcessColumn::CpuPercent, "Sparkline");
    EXPECT_EQ(UserConfig::get().settings().processMeters.style(ProcessColumn::CpuPercent), ProcessRowMeter::Style::Sparkline);
    EXPECT_NE(panel->processModel()->rowHistoryMetrics(), 0U);
    pressStyle(ProcessColumn::CpuPercent, "Bar");
    EXPECT_EQ(panel->processModel()->rowHistoryMetrics(), 0U);
    UserConfig::get().settings().processMeters = ProcessRowMeter::Settings{}; // The singleton outlives this test
    panel->onDetach();
}

} // namespace
} // namespace App
