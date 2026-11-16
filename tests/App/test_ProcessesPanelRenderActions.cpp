/// @file test_ProcessesPanelRenderActions.cpp
/// @brief The Processes panel run headless (#1546): Ctrl+F and the keyboard actions, changes held while Process Details applies its own,
/// Efficiency mode retries, actions on a selection, pinning, and batch priority and I/O priority. Split from test_ProcessesPanelRender.cpp
/// by feature (#3850); the fixture is in ProcessesPanelRenderFixture.h.

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

// Ctrl+F over the table puts the cursor in the filter box: what's typed next filters the rows.
TEST_F(ProcessesPanelRenderTest, CtrlFPutsTheCursorInTheFilter)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // hovered: the table's keys are armed
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_F, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddKeyEvent(ImGuiKey_F, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr); // not Find handles
    for (const char* character : {"w", "o", "r", "k"})
    {
        io.AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
}

// #3186: the Group control picks a mode (leaving Tree view, which grouping replaces); the keyboard and
// Ctrl+A walk the grouped rows; a header clicked twice collapses its group and opens it again.
TEST_F(ProcessesPanelRenderTest, TheGroupControlLeavesTreeViewAndGroupedRowsNavigate)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_SITEMAP " Tree", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));

    ImGui::ActivateItemByID(ImHashStr("##GroupBy", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressPopupItem(*panel, "Unit");
    std::string text = renderAndCapture(*panel);
    EXPECT_EQ(UserConfig::get().settings().processGroupBy, ProcessGrouping::GroupBy::Unit);
    EXPECT_TRUE(text.contains("Group: Unit")) << text;
    EXPECT_TRUE(text.contains("Ungrouped (3)")) << text; // none of the mock processes has a unit

    // The keyboard moves through the processes under the headers, and Ctrl+A selects all of them.
    pressKey(*panel, ImGuiKey_DownArrow);
    ASSERT_FALSE(events.selected.empty());
    pressKey(*panel, ImGuiKey_DownArrow);
    EXPECT_EQ(events.selected.size(), 2U);
    selectAll(*panel);
    EXPECT_EQ(panel->selectionCount(), 3U);

    // The header row, clicked once, collapses its group; clicked again, opens it.
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
    const ImVec2 header((table->Columns[1].MinX + table->Columns[1].MaxX) * 0.5F, table->OuterRect.Min.y + (rowHeight * 1.5F));
    ImGuiIO& io = ImGui::GetIO();
    for (const bool open : {false, true})
    {
        io.AddMousePosEvent(header.x, header.y);
        static_cast<void>(renderAndCapture(*panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderAndCapture(*panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderAndCapture(*panel));
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        text = renderAndCapture(*panel);
        EXPECT_EQ(text.contains("worker"), open) << text;
    }
    panel->onDetach(); // TearDown restores the settings
}

// #3097: F7 sets the selected process's priority one step higher at once (no confirm), on the row action
// worker, and says so in the toolbar; F8 one step lower.
TEST_F(ProcessesPanelRenderTest, F7AndF8StepTheSelectedProcessesPriorityAtOnce)
{
    SelectionSink events;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions(events.sink(), actions);
    renderUntilLoaded(*panel);
    ASSERT_NE(actions, nullptr);
    pressKey(*panel, ImGuiKey_DownArrow);
    const std::int32_t pid = panel->selectedPid();
    ASSERT_GT(pid, 0);

    for (const NiceStep::Direction direction : {NiceStep::Direction::Raise, NiceStep::Direction::Lower})
    {
        const int before = actions->setPriorityCount();
        panel->requestPriorityStep(direction);
        static_cast<void>(renderAndCapture(*panel));
        panel->waitForRowActionForTesting();
        const std::string text = renderAndCapture(*panel);
        EXPECT_EQ(actions->setPriorityCount(), before + 1);
        EXPECT_EQ(actions->lastSetPriorityPid(), pid);
        // The mock's processes are at nice 0, with no class read.
        EXPECT_EQ(actions->lastSetPriorityNice(), NiceStep::next(0, Domain::Priority::PriorityClass::None, direction).value_or(99));
        EXPECT_TRUE(text.contains("Priority of")) << text;
    }

    // No selection: nothing happens.
    panel->requestPriorityStep(NiceStep::Direction::Raise);
    panel->expireFrameRequests(); // the frame ended before the table took it
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(actions->setPriorityCount(), 2);
}

// #3467, Process Details to the table: while Process Details changes the selected process's priority, F7 sets
// nothing and the toolbar says why; once that change has finished, F7 works again. A process that merely has
// the same PID (another start time) is not held.
TEST_F(ProcessesPanelRenderTest, F7WaitsForAPriorityChangeInProcessDetailsForThatProcess)
{
    PriorityChangesInFlight changes;
    SelectionSink events;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions(events.sink(), actions);
    panel->setPriorityChanges(&changes);
    renderUntilLoaded(*panel);
    ASSERT_NE(actions, nullptr);
    pressKey(*panel, ImGuiKey_DownArrow);
    const std::int32_t pid = panel->selectedPid();
    ASSERT_GT(pid, 0);
    const Platform::ProcessTarget target{.pid = pid, .startTimeTicks = mockStartTicks(pid)};

    const std::array details{target};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);
    panel->requestPriorityStep(NiceStep::Direction::Raise);
    static_cast<void>(renderAndCapture(*panel));
    const std::string refused = renderAndCapture(*panel);
    EXPECT_EQ(actions->setPriorityCount(), 0) << "held while Process Details changes it";
    EXPECT_TRUE(refused.contains(PriorityChangesInFlight::refusal(PriorityChangesInFlight::Panel::Processes))) << refused;

    // The same PID, another start time: another process, so not held.
    const std::array reused{Platform::ProcessTarget{.pid = pid, .startTimeTicks = target.startTimeTicks + 1}};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, reused);
    panel->requestPriorityStep(NiceStep::Direction::Raise);
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setPriorityCount(), 1);
    EXPECT_EQ(actions->lastTarget(), target);

    // Released: F7 sets it again.
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, {});
    panel->requestPriorityStep(NiceStep::Direction::Lower);
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->setPriorityCount(), 2);
    panel->onDetach();
}

// #3467, the table to Process Details: F7's change is published for its process from the frame it starts until
// its result is taken in, so Process Details holds its priority controls for that process meanwhile.
TEST_F(ProcessesPanelRenderTest, F7HoldsProcessDetailsPriorityControlsUntilItsResultIsTakenIn)
{
    PriorityChangesInFlight changes;
    SelectionSink events;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions(events.sink(), actions);
    panel->setPriorityChanges(&changes);
    renderUntilLoaded(*panel);
    ASSERT_NE(actions, nullptr);
    pressKey(*panel, ImGuiKey_DownArrow);
    const std::int32_t pid = panel->selectedPid();
    ASSERT_GT(pid, 0);
    const Platform::ProcessTarget target{.pid = pid, .startTimeTicks = mockStartTicks(pid)};
    EXPECT_FALSE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target));

    panel->requestPriorityStep(NiceStep::Direction::Raise);
    static_cast<void>(renderAndCapture(*panel)); // started on the row action worker
    EXPECT_TRUE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target)) << "held from the frame it starts";
    EXPECT_FALSE(
        changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, {.pid = pid, .startTimeTicks = target.startTimeTicks + 1}))
        << "a reused PID is another process";

    ASSERT_TRUE(updateUntilBatchResult(*panel, "Priority of")); // onUpdate() takes the result in
    EXPECT_EQ(actions->setPriorityCount(), 1);
    EXPECT_FALSE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target)) << "released once taken in";
    panel->onDetach();
}

// #3467: Set priority for N processes... leaves out a process Process Details is changing the priority of, sets the
// others, and its result line says how many it skipped and why.
TEST_F(ProcessesPanelRenderTest, ABatchPriorityChangeSkipsAProcessWithAChangeRunningInProcessDetails)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    const Platform::ProcessTarget server{.pid = 100, .startTimeTicks = mockStartTicks(100)};
    const std::array details{server};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);

    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, std::format("{} Set priority for 3 processes...###SetPriority", ICON_FA_GAUGE_HIGH));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    pressModalFilledButton(*panel, "Apply to 3 processes");
    ASSERT_TRUE(updateUntilBatchResult(*panel, "1 process skipped")); // from the batch's worker (#2142)
    EXPECT_EQ(actions->setPriorityCount(), 2) << "the other two are set";
    EXPECT_NE(actions->lastTarget(), server);
    panel->onDetach();
}

// #3467: the row menu's Set Realtime priority... is greyed out while Process Details changes that process's
// priority; once that has finished it asks again, and its confirm and its run hold Process Details' controls
// for that process until the result is taken in.
TEST_F(ProcessesPanelRenderTest, SetRealtimeFromTheRowMenuAndProcessDetailsHoldEachOther)
{
    PriorityChangesInFlight changes;
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
    panel->setPriorityChanges(&changes);
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);

    std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::int32_t pid = menuPid.value_or(-1);
    const Platform::ProcessTarget target{.pid = pid, .startTimeTicks = mockStartTicks(pid)};
    const std::array details{target};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);
    static_cast<void>(renderAndCapture(*panel));
    pressPopupItem(*panel, App::Detail::SET_REALTIME_PRIORITY_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr) << "greyed out while Process Details changes it";

    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, {});
    if (topPopup() != nullptr)
    {
        closePopups(*panel);
    }
    menuPid = openARowMenu(*panel);
    ASSERT_EQ(menuPid, target.pid);
    pressPopupItem(*panel, App::Detail::SET_REALTIME_PRIORITY_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "released: it asks";
    EXPECT_TRUE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target)) << "its confirm holds Process Details";

    pressModalFilledButton(*panel, "Set Realtime priority");
    EXPECT_TRUE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target)) << "and so does its run";
    ASSERT_TRUE(updateUntilBatchResult(*panel, "set to Realtime"));
    EXPECT_EQ(actions->setPriorityCount(), 1);
    EXPECT_FALSE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, target)) << "released once taken in";
    panel->onDetach();
}

// #3467 review: Efficiency mode sets the priority too (Idle on Windows). The row menu's item is greyed out while
// Process Details changes that process's priority; once that has finished it asks again, and its confirm and its
// run hold Process Details' priority controls for that process until the result is taken in.
TEST_F(ProcessesPanelRenderTest, EfficiencyModeFromTheRowMenuAndProcessDetailsHoldEachOther)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = efficiencyModePanel("", actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    constexpr auto DETAILS = PriorityChangesInFlight::Panel::ProcessDetails;

    std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::int32_t pid = menuPid.value_or(-1);
    const Platform::ProcessTarget target{.pid = pid, .startTimeTicks = mockStartTicks(pid)};
    const std::array details{target};
    changes.publish(DETAILS, details);
    static_cast<void>(renderAndCapture(*panel));
    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr) << "greyed out while Process Details changes its priority";
    EXPECT_EQ(actions->efficiencyModeCount(), 0);

    changes.publish(DETAILS, {});
    if (topPopup() != nullptr)
    {
        closePopups(*panel);
    }
    menuPid = openARowMenu(*panel);
    ASSERT_EQ(menuPid, target.pid);
    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "released: it asks";
    EXPECT_TRUE(changes.heldElsewhere(DETAILS, target)) << "its confirm holds Process Details";

    pressModalButton(*panel, "Turn on Efficiency mode");
    EXPECT_TRUE(changes.heldElsewhere(DETAILS, target)) << "and so does its run";
    // Waits for the row action worker and takes its result in, as the next frame's onUpdate() would; that
    // frame's render then publishes the release. Not a text match: the progress line ("Turning Efficiency mode
    // on for PID 1...") reads like the result's.
    panel->waitForRowActionForTesting();
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains(std::format("Efficiency mode on for PID {}", target.pid))) << text;
    EXPECT_FALSE(text.contains("Turning Efficiency mode on")) << text;
    EXPECT_EQ(actions->efficiencyModeCount(), 1);
    EXPECT_FALSE(changes.heldElsewhere(DETAILS, target)) << "released once taken in";
    panel->onDetach();
}

// #3467 review: a refused Efficiency mode change's "Authenticate and retry" in the toolbar isn't offered while
// Process Details changes that process's priority (an older retry could overwrite the newer change), and is
// offered again once that has finished.
TEST_F(ProcessesPanelRenderTest, ARefusedEfficiencyModeChangesRetryWaitsForProcessDetails)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = efficiencyModePanel(std::string(Platform::EFFICIENCY_MODE_STATUS), actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    actions->setCapabilities({.canTerminate = true, .canKill = true, .canAuthorizeInteractively = true, .canSetEfficiencyMode = true});
    actions->setEfficiencyModeResult(Platform::ProcessActionResult::denied("Access denied"));

    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());
    const std::int32_t pid = menuPid.value_or(-1);
    const Platform::ProcessTarget target{.pid = pid, .startTimeTicks = mockStartTicks(pid)};
    pressPopupItem(*panel, App::Detail::EFFICIENCY_MODE_LABEL); // in it: off at once, refused
    panel->waitForRowActionForTesting();
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(panel->offersAuthenticateAndRetry()) << "refused: the retry is offered";

    const std::array details{target};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);
    EXPECT_FALSE(panel->offersAuthenticateAndRetry()) << "held while Process Details changes its priority";
    panel->retryRowActionAuthorizedForTesting();
    EXPECT_EQ(actions->runAuthorizedCount(), 0) << "a press sends nothing meanwhile";

    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, {});
    EXPECT_TRUE(panel->offersAuthenticateAndRetry()) << "offered again once it has finished";
    panel->onDetach();
}

// #3097: "/" over the table opens the Find box; typing jumps the selection to the first matching row,
// Enter to the next; a name nothing has says so; Escape closes the box.
TEST_F(ProcessesPanelRenderTest, SlashFindsAProcessByNameAndEnterGoesToTheNext)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    pressKey(*panel, ImGuiKey_Slash);
    static_cast<void>(renderAndCapture(*panel));
    ImGuiIO& io = ImGui::GetIO();
    for (const char* character : {"e", "r"}) // "server" and "worker"
    {
        io.AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::int32_t first = panel->selectedPid();
    EXPECT_TRUE(first == 100 || first == 101) << first;

    io.AddKeyEvent(ImGuiKey_Enter, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddKeyEvent(ImGuiKey_Enter, false);
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const std::int32_t second = panel->selectedPid();
    EXPECT_TRUE(second == 100 || second == 101) << second;
    EXPECT_NE(second, first);

    for (const char* character : {"z", "z"})
    {
        io.AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("No match"));
    EXPECT_EQ(panel->selectedPid(), second); // the selection stays where it was

    io.AddKeyEvent(ImGuiKey_Escape, true);
    static_cast<void>(renderAndCapture(*panel));
    io.AddKeyEvent(ImGuiKey_Escape, false);
    static_cast<void>(renderAndCapture(*panel));
    const std::string closed = renderAndCapture(*panel);
    EXPECT_FALSE(closed.contains("No match")) << closed;
    EXPECT_FALSE(closed.contains("Find a name")) << closed;
}

// #3097: F6 opens the "Sort by" menu of the visible columns; picking Name sorts by it, and picking it
// again reverses the order.
TEST_F(ProcessesPanelRenderTest, F6SortMenuPicksTheSortColumnAndPickingItAgainReversesIt)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const auto initBeforeWorker = [&]
    {
        const std::string text = renderAndCapture(*panel);
        return text.find("init") < text.find("worker");
    };
    EXPECT_FALSE(initBeforeWorker());         // the default sort: PID, highest first
    for (const bool expected : {true, false}) // Name A to Z, then Z to A
    {
        panel->requestSortMenu();
        static_cast<void>(renderAndCapture(*panel));
        static_cast<void>(renderAndCapture(*panel));
        ASSERT_NE(topPopup(), nullptr) << "F6 didn't open the Sort by menu";
        EXPECT_TRUE(renderAndCapture(*panel).contains("Sort by"));
        pressPopupItem(*panel, "Name");
        static_cast<void>(renderAndCapture(*panel));
        EXPECT_EQ(initBeforeWorker(), expected);
    }
}

// #3221: F7 on a process already at the highest priority, and F8 on one at the lowest, set nothing and
// say so in the toolbar. Each process is at the end of both scales, nice and (Windows) class.
TEST_F(ProcessesPanelRenderTest, F7AndF8AtTheEndOfThePriorityRangeSayWhy)
{
    for (const NiceStep::Direction direction : {NiceStep::Direction::Raise, NiceStep::Direction::Lower})
    {
        const bool raise = direction == NiceStep::Direction::Raise;
        TestMocks::MockProcessActions* actions = nullptr;
        const auto panel = std::make_unique<ProcessesPanel>(
            [&actions, raise]
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                for (const std::int32_t pid : {1, 100, 101})
                {
                    auto process =
                        TestMocks::makeProcessCounters(pid, std::format("p{}", pid), 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0);
                    process.nice = raise ? Domain::Priority::MIN_NICE : Domain::Priority::MAX_NICE;
                    process.priorityClass = raise ? Platform::PriorityClass::Realtime : Platform::PriorityClass::Idle;
                    probe->withProcess(process);
                }
                auto mock = std::make_unique<TestMocks::MockProcessActions>();
                mock->setCapabilities({.canSetPriority = true});
                actions = mock.get();
                return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
            },
            [](Core::Event& /*event*/) {});
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        renderUntilLoaded(*panel);
        ASSERT_NE(actions, nullptr);
        pressKey(*panel, ImGuiKey_DownArrow);
        ASSERT_GT(panel->selectedPid(), 0);

        panel->requestPriorityStep(direction);
        static_cast<void>(renderAndCapture(*panel));
        const std::string text = renderAndCapture(*panel);
        EXPECT_EQ(actions->setPriorityCount(), 0);
        EXPECT_TRUE(text.contains(raise ? "is already at the highest priority" : "is already at the lowest priority")) << text;
        panel->onDetach();
    }
}

// #3221: Suspend and Resume from the row menu on a selection ask once for all of it, before anything
// is sent.
TEST_F(ProcessesPanelRenderTest, SuspendAndResumeFromTheRowMenuOnASelectionAreConfirmedFirst)
{
    for (const Detail::ProcessAction action : {Detail::ProcessAction::Stop, Detail::ProcessAction::Resume})
    {
        TestMocks::MockProcessActions* actions = nullptr;
        const auto panel = std::make_unique<ProcessesPanel>(
            [&actions]
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                    .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                    .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
                auto mock = std::make_unique<TestMocks::MockProcessActions>();
                mock->setCapabilities({.canStop = true, .canContinue = true});
                actions = mock.get();
                return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
            },
            [](Core::Event& /*event*/) {});
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        renderUntilLoaded(*panel);
        ASSERT_NE(actions, nullptr);
        selectAll(*panel);
        ASSERT_TRUE(openARowMenu(*panel).has_value());
        pressPopupItem(*panel, std::format("###{}", Detail::actionLabel(action)));
        static_cast<void>(renderAndCapture(*panel));
        static_cast<void>(renderAndCapture(*panel));
        const ImGuiWindow* confirm = ImGui::GetTopMostPopupModal();
        ASSERT_NE(confirm, nullptr) << Detail::actionLabel(action) << " is confirmed";
        EXPECT_TRUE(std::string_view(confirm->Name).contains("3 processes")) << confirm->Name;
        pressModalButton(*panel, "Cancel");
        panel->onDetach();
    }
}

// #3221: a panel that was never attached has no model or sampler: it counts and watches nothing, its
// interval is the one it was given, and updates and events leave it as it is.
TEST_F(ProcessesPanelRenderTest, APanelThatWasNeverAttachedIgnoresUpdatesAndEvents)
{
    ProcessesPanel panel(
        []
        { return ProcessesPanelPlatform{.probe = std::make_unique<TestMocks::MockProcessProbe>(), .actions = nullptr, .icons = nullptr}; },
        [](Core::Event& /*event*/) {});
    panel.setSamplingInterval(std::chrono::milliseconds(750), false);
    EXPECT_EQ(panel.samplingInterval(), std::chrono::milliseconds(750));
    EXPECT_EQ(panel.processCount(), 0U);
    panel.watchProcess(42);
    std::vector<Domain::ProcessSample> samples;
    EXPECT_FALSE(panel.watchedSamplesSince(0, samples));
    panel.onUpdate(1.0F / 60.0F);
    Core::HistoryDurationChangedEvent history(120);
    panel.onEvent(history);
    EXPECT_EQ(panel.processCount(), 0U);
}

// #3221: a group's header totals each column in its own unit, marked with Sigma: rates, counts, power and
// bytes alike.
TEST_F(ProcessesPanelRenderTest, GroupHeadersTotalEachColumnInItsUnit)
{
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::Container;
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col :
         {ProcessColumn::IoRead, ProcessColumn::NetSent, ProcessColumn::Threads, ProcessColumn::Handles, ProcessColumn::Power})
    {
        columns.setVisible(col, true);
    }
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            auto web = TestMocks::makeProcessCounters(100, "nginx", 'S', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            web.container = "docker:1a2b3c4d5e6f";
            web.threadCount = 4;
            web.handleCount = 40;
            Platform::ProcessCapabilities caps;
            caps.hasContainer = true; // Group: Container is offered only where containers are read (#3428)
            caps.hasThreadCount = true;
            caps.hasHandleCount = true;
            caps.hasIoCounters = true;
            caps.hasNetworkCounters = true;
            caps.hasPowerUsage = true;
            probe->setCapabilities(caps);
            probe->withProcess(web);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        [](Core::Event& /*event*/) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < 1; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("docker:1a2b3c4d5e6f (1)")) << text;
    EXPECT_TRUE(text.contains("\xCE\xA3 4")) << "the threads' total, a count: " << text;
    panel->onDetach(); // TearDown restores the settings
}

// #3063: where the platform can send the other signals (Linux), the row menu's Send signal submenu
// lists them, and SIGHUP is confirmed, then sent to that process.
TEST_F(ProcessesPanelRenderTest, SendSignalFromTheRowMenuIsConfirmedThenSent)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = panelWithCapabilities({.canKill = true, .canSendSignals = true}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    const std::optional<std::int32_t> menuPid = openARowMenu(*panel);
    ASSERT_TRUE(menuPid.has_value());

    const ImGuiWindow* menu = topPopup();
    ASSERT_NE(menu, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_BOLT " Send signal###SendSignal", 0, menu->ID));
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    ASSERT_NE(topPopup(), menu) << "the submenu opened over the row menu";
    for (const char* name : {"SIGHUP", "SIGINT", "SIGQUIT", "SIGUSR1", "SIGUSR2"})
    {
        EXPECT_TRUE(text.contains(name)) << name << "\n" << text;
    }
    pressPopupItem(*panel, "SIGHUP...###SIGHUP");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_EQ(actions->sendSignalCount(), 0); // asked, not done
    pressModalButton(*panel, "SIGHUP");
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->sendSignalCount(), 1);
    EXPECT_EQ(actions->lastSignal(), Platform::ProcessSignal::Hangup);
    panel->onDetach();
}

// #3061: with ProcessPins::MAX_PINS names pinned already, Pin to top pins nothing more and says why.
TEST_F(ProcessesPanelRenderTest, PinningPastTheLimitSaysWhy)
{
    std::vector<std::string> pinned;
    pinned.reserve(ProcessPins::MAX_PINS);
    for (std::size_t i = 0; i < ProcessPins::MAX_PINS; ++i)
    {
        pinned.push_back(std::format("pinned-{}", i));
    }
    UserConfig::get().settings().pinnedProcesses = pinned;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, ICON_FA_THUMBTACK " Pin to top###Pin");
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains(std::format("Can't pin more than {} processes", ProcessPins::MAX_PINS))) << text;
    EXPECT_EQ(UserConfig::get().settings().pinnedProcesses, pinned) << "nothing more pinned";
    panel->onDetach();
    UserConfig::get().settings().pinnedProcesses.clear();
}

// #1492: the UDP event trace's switch reaches the panel as an application event; the panel turns the
// trace on and off on the switch's worker, never in the dispatch, and leaves the event for others.
TEST_F(ProcessesPanelRenderTest, TheUdpEventTraceSwitchIsTakenAndPassedOn)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    for (const bool enabled : {true, false})
    {
        Core::UdpEventTraceChangedEvent changed(enabled);
        panel->onEvent(changed);
        EXPECT_FALSE(changed.isHandled()) << "other listeners see it too";
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
    }
    EXPECT_EQ(panel->processCount(), 3U);
    panel->onDetach();
}

// Process Details looks its process up in the model's latest snapshot, not the table's cached rows
// (findSnapshot()): a listed PID is found, an unknown one isn't, and a panel without a model finds none.
// #3061: a pinned process stays in view under a filter it doesn't match.
TEST_F(ProcessesPanelRenderTest, APinnedProcessStaysInViewUnderAFilter)
{
    UserConfig::get().settings().pinnedProcesses = {"init"};
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
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
    EXPECT_TRUE(text.contains("init")) << "pinned, so kept: " << text;
    EXPECT_FALSE(text.contains("server")) << text;
    panel->onDetach();
    UserConfig::get().settings().pinnedProcesses.clear();
}

// A process name longer than ProcessPins::MAX_NAME_BYTES can't be pinned: the row menu's Pin to top is
// disabled, and choosing it pins nothing.
TEST_F(ProcessesPanelRenderTest, AnOverlongNameCantBePinned)
{
    UserConfig::get().settings().pinnedProcesses.clear();
    const std::string longName(ProcessPins::MAX_NAME_BYTES + 1, 'n');
    const auto panel = std::make_unique<ProcessesPanel>(
        // NOLINTNEXTLINE(bugprone-exception-escape): MSVC's debug std::string copy allocates its container proxy; a throw there is a test failure
        [longName]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(9, longName, 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        [](Core::Event& /*event*/) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < 1; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_EQ(openARowMenu(*panel), std::optional<std::int32_t>{9});
    pressPopupItem(*panel, ICON_FA_THUMBTACK " Pin to top###Pin");
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(UserConfig::get().settings().pinnedProcesses.empty());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, FindSnapshotReadsTheModelsLatestSnapshot)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::shared_ptr<const Domain::ProcessSnapshot> server = panel->findSnapshot(100);
    ASSERT_NE(server, nullptr);
    EXPECT_EQ(server != nullptr ? server->name : std::string{}, "server");
    EXPECT_EQ(panel->findSnapshot(424242), nullptr);
    panel->onDetach();
    EXPECT_EQ(panel->findSnapshot(100), nullptr) << "detached: no model";
}

/// Mock actions that say when they are destroyed.
class TrackedActions : public TestMocks::MockProcessActions
{
  public:
    explicit TrackedActions(std::shared_ptr<std::atomic<bool>> destroyed) : m_Destroyed(std::move(destroyed))
    {}
    TrackedActions(const TrackedActions&) = delete;
    TrackedActions& operator=(const TrackedActions&) = delete;
    TrackedActions(TrackedActions&&) = delete;
    TrackedActions& operator=(TrackedActions&&) = delete;
    ~TrackedActions() override
    {
        m_Destroyed->store(true);
    }

  private:
    std::shared_ptr<std::atomic<bool>> m_Destroyed;
};

// A panel detached straight after attach, before any frame adopted the platform its sampler built: the
// handed-over actions are taken from the handoff and kept with the panel's own, so they go with the panel.
TEST_F(ProcessesPanelRenderTest, APlatformNoFrameAdoptedIsReleasedOnDetach)
{
    const auto destroyed = std::make_shared<std::atomic<bool>>(false);
    auto panel = std::make_unique<ProcessesPanel>(
        [destroyed]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TrackedActions>(destroyed),
                .icons = nullptr,
            };
        },
        [](Core::Event& /*event*/) {});
    attachAndSeed(*panel);
    EXPECT_FALSE(destroyed->load());
    panel->onDetach(); // no onUpdate() or frame in between: nothing adopted the handoff
    EXPECT_FALSE(destroyed->load()) << "taken from the handoff, not dropped with it";
    panel.reset();
    EXPECT_TRUE(destroyed->load());
}

// TASKSMACK_SELECT (#1559) for a process that never appears: after SelectOverride::MAX_SNAPSHOTS
// snapshots it gives up, selecting nothing, rather than waiting for ever.
TEST_F(ProcessesPanelRenderTest, AStartupSelectionThatNeverMatchesGivesUp)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    panel->requestStartupSelection(SelectOverride::Target{.pid = 424242, .name = {}, .tab = {}}, true);
    updateUntilLoaded(*panel);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (panel->startupSelectionPending() && std::chrono::steady_clock::now() < deadline)
    {
        panel->requestRefresh(); // a new snapshot, rather than waiting a refresh interval for each
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_FALSE(panel->startupSelectionPending());
    EXPECT_EQ(panel->selectionCount(), 0U);
    EXPECT_EQ(events.showDetails, 0);
    EXPECT_FALSE(panel->takeStartupDetailsTab().has_value());
    panel->onDetach();
}

// #2308: the toolbar's Authenticate and retry button, pressed, runs the refused action again through
// the helper, as its shortcut does.
TEST_F(ProcessesPanelRenderTest, TheToolbarsRetryButtonRunsTheRefusedActionAuthorized)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    actions->setCapabilities({.canTerminate = true, .canKill = true, .canAuthorizeInteractively = true});
    actions->setKillResult(Platform::ProcessActionResult::denied("Permission denied - process belongs to another user"));
    renderUntilLoaded(*panel);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, std::format("{} Kill...###Kill", ICON_FA_SKULL));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalFilledButton(*panel, "Kill");
    panel->waitForRowActionForTesting();
    ASSERT_TRUE(panel->offersAuthenticateAndRetry());
    static_cast<void>(renderAndCapture(*panel));

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(AuthorizeWording::RETRY_LABEL, 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    panel->waitForRowActionForTesting();
    EXPECT_EQ(actions->runAuthorizedCount(), 1);
    EXPECT_EQ(actions->lastAuthorizedAction().kind, Platform::AuthorizedAction::Kind::Kill);
    panel->onDetach();
}

// #3067: the toolbar's magnifier button opens Find handles, as Ctrl+Shift+F does.
TEST_F(ProcessesPanelRenderTest, TheToolbarsFindHandlesButtonOpensTheDialog)
{
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
                .openFiles = std::make_unique<TestMocks::MockProcessOpenFilesReader>(),
            };
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
    ASSERT_EQ(ImGui::GetTopMostPopupModal(), nullptr);

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_MAGNIFYING_GLASS "###FindHandles", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr) << "the button didn't open Find handles";
    EXPECT_TRUE(renderAndCapture(*panel).contains("Type a name and press Find."));
    panel->onDetach();
}

// #3086: the Scheduling Policy column shows each process's policy where the platform reads policies
// (Linux), and the dash for a process whose policy couldn't be read.
TEST_F(ProcessesPanelRenderTest, ThePolicyColumnShowsEachProcesssPolicyOrTheDash)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Policy, true);
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters realTime =
                TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            realTime.schedPolicy = Platform::SchedPolicy::Fifo;
            realTime.rtPriority = 50;
            probe->withProcess(std::move(realTime))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasSchedPolicy = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < 2; ++frame)
    {
        static_cast<void>(renderAndCapture(*panel));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("FIFO 50")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "worker's policy wasn't read:\n" << text;
    panel->onDetach(); // TearDown restores the settings
}

/// Whether any vertex in the last frame's draw data has colour @p color.
[[nodiscard]] bool frameDrewColor(ImU32 color)
{
    for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
    {
        for (const ImDrawVert& vertex : list->VtxBuffer)
        {
            if (vertex.col == color)
            {
                return true;
            }
        }
    }
    return false;
}

// #3097: a process that stays in uninterruptible sleep (D) for StuckProcesses::STUCK_SECONDS has its row
// tinted in the theme's disk-sleep colour; until then it isn't.
TEST_F(ProcessesPanelRenderTest, AProcessStuckInDiskSleepHasItsRowTinted)
{
    const auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(77, "nfs-reader", 'D', 30, 10, 200, std::uint64_t{2} << 20U, 1));
            return ProcessesPanelPlatform{
                .probe = std::move(probe),
                .actions = std::make_unique<TestMocks::MockProcessActions>(),
                .icons = nullptr,
            };
        },
        [](Core::Event& /*event*/) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    ImVec4 tint = UI::Theme::get().scheme().statusDiskSleep;
    tint.w = StuckProcesses::TINT_ALPHA;
    const ImU32 tintColor = ImGui::GetColorU32(tint);
    panel->onUpdate(1.0F / 60.0F);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(frameDrewColor(tintColor)) << "not stuck yet";

    // New generations until it has been in D long enough.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    bool tinted = false;
    while (!tinted && std::chrono::steady_clock::now() < deadline)
    {
        panel->requestRefresh();
        panel->onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(*panel));
        tinted = frameDrewColor(tintColor);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(tinted);
    panel->onDetach();
}

/// An attached panel reading init (1), server (100) and worker (101), with I/O priorities read for the first two
/// (#3551) and none for the worker, over mock actions that can set an I/O priority; @p actions is left pointing at them.
[[nodiscard]] std::unique_ptr<ProcessesPanel> ioPriorityPanel(TestMocks::MockProcessActions*& actions)
{
    auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasIoPriority = true;
            probe->setCapabilities(caps);
            auto init = TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0);
            init.ioPriority = Platform::IoPriority{.ioClass = Platform::IoPriorityClass::Idle, .level = 0};
            auto server = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            server.ioPriority = Platform::IoPriority{.ioClass = Platform::IoPriorityClass::BestEffort, .level = 2};
            auto worker = TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100);
            for (const auto& counters : {init, server, worker})
            {
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canSetIoPriority = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    return panel;
}

// #3551, #3374: Set I/O priority for N processes... sets it on every process the dialog listed when Apply is pressed,
// on the batch's worker, and the toolbar names the priority and the count.
TEST_F(ProcessesPanelRenderTest, TheBatchIoPriorityDialogsApplySetsItOnEveryListedProcess)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = ioPriorityPanel(actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, "###SetIoPriority");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(panel->batchIoPriorityDialogForTesting().isPending());
    pressModalFilledButton(*panel, "Apply to 3 processes");
    ASSERT_TRUE(updateUntilBatchResult(*panel, "I/O priority set to")); // from the batch's worker (#2142)
    EXPECT_TRUE(renderAndCapture(*panel).contains("for 3 processes"));
    EXPECT_EQ(actions->setIoPriorityCount(), 3);
    panel->onDetach();
}

// #3884: Set I/O priority for N processes... leaves out a process Process Details is changing the I/O priority of,
// sets the others, and its result line says how many it skipped and why, as the nice batch does (#3467).
TEST_F(ProcessesPanelRenderTest, ABatchIoPriorityChangeSkipsAProcessWithAnIoChangeRunningInProcessDetails)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = ioPriorityPanel(actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    const Platform::ProcessTarget server{.pid = 100, .startTimeTicks = mockStartTicks(100)};
    const std::array details{server};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details, PriorityChangesInFlight::Change::IoPriority);

    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, "###SetIoPriority");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(panel->batchIoPriorityDialogForTesting().isPending());
    pressModalFilledButton(*panel, "Apply to 3 processes");
    ASSERT_TRUE(updateUntilBatchResult(*panel, "1 process skipped")); // from the batch's worker (#2142)
    EXPECT_TRUE(renderAndCapture(*panel).contains("I/O priority change is still running in Process Details"));
    EXPECT_EQ(actions->setIoPriorityCount(), 2) << "the other two are set";
    panel->onDetach();
}

// #3884: a CPU priority change running in Process Details doesn't hold the batch I/O priority change, which sets a
// different thing: every listed process is set.
TEST_F(ProcessesPanelRenderTest, ACpuPriorityChangeInProcessDetailsDoesNotHoldTheBatchIoPriorityChange)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = ioPriorityPanel(actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    const std::array details{Platform::ProcessTarget{.pid = 100, .startTimeTicks = mockStartTicks(100)}};
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);

    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, "###SetIoPriority");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    pressModalFilledButton(*panel, "Apply to 3 processes");
    ASSERT_TRUE(updateUntilBatchResult(*panel, "I/O priority set to"));
    EXPECT_EQ(actions->setIoPriorityCount(), 3);
    panel->onDetach();
}

// #3884: the batch I/O priority dialog is its confirmation, so its processes are published as I/O priority changes
// from the frame it opens, holding Process Details' I/O priority Apply for them, and not its CPU priority controls.
TEST_F(ProcessesPanelRenderTest, TheBatchIoPriorityDialogPublishesItsProcessesAsIoPriorityChanges)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = ioPriorityPanel(actions);
    panel->setPriorityChanges(&changes);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    const Platform::ProcessTarget server{.pid = 100, .startTimeTicks = mockStartTicks(100)};
    EXPECT_FALSE(
        changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, server, PriorityChangesInFlight::Change::IoPriority));
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, "###SetIoPriority");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_TRUE(panel->batchIoPriorityDialogForTesting().isPending());
    EXPECT_TRUE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, server, PriorityChangesInFlight::Change::IoPriority));
    EXPECT_FALSE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, server))
        << "an I/O priority change holds no CPU priority control";
    panel->onDetach();
    EXPECT_FALSE(changes.heldElsewhere(PriorityChangesInFlight::Panel::ProcessDetails, server, PriorityChangesInFlight::Change::IoPriority))
        << "released at detach";
}

// #3551, #3374: the I/O priority column shows each process's class and level as read, and a process whose I/O
// priority couldn't be read gets the unavailable cell, not a made-up value.
TEST_F(ProcessesPanelRenderTest, TheIoPriorityColumnShowsWhatWasReadAndMarksWhatWasnt)
{
    ProcessColumnSettings& settings = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : allProcessColumns())
    {
        settings.setVisible(col, col == ProcessColumn::IoPriority);
    }
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = ioPriorityPanel(actions);
    renderUntilLoaded(*panel);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains(getColumnInfo(ProcessColumn::IoPriority).name)) << text;
    EXPECT_TRUE(text.contains("best-effort 2")) << text;
    EXPECT_TRUE(text.contains("idle")) << text;
    EXPECT_FALSE(text.contains("default")) << "the worker's unread priority isn't worded as one:\n" << text;
    panel->onDetach();
}

// #3467, #3374: when every selected process has a priority change running in Process Details, Set priority for N
// processes... sets none, starts no batch, and says why at once.
TEST_F(ProcessesPanelRenderTest, ABatchPriorityChangeWithEveryProcessHeldElsewhereSetsNone)
{
    PriorityChangesInFlight changes;
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    panel->setPriorityChanges(&changes);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);
    const std::array details{
        Platform::ProcessTarget{.pid = 1, .startTimeTicks = mockStartTicks(1)},
        Platform::ProcessTarget{.pid = 100, .startTimeTicks = mockStartTicks(100)},
        Platform::ProcessTarget{.pid = 101, .startTimeTicks = mockStartTicks(101)},
    };
    changes.publish(PriorityChangesInFlight::Panel::ProcessDetails, details);

    ASSERT_TRUE(openARowMenu(*panel).has_value());
    pressPopupItem(*panel, std::format("{} Set priority for 3 processes...###SetPriority", ICON_FA_GAUGE_HIGH));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    pressModalFilledButton(*panel, "Apply to 3 processes");
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Priority not set; 3 processes skipped"));
    EXPECT_EQ(actions->setPriorityCount(), 0);
    panel->onDetach();
}

} // namespace
} // namespace App
