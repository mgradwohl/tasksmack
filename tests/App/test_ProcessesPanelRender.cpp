/// @file test_ProcessesPanelRender.cpp
/// @brief The Processes panel as a whole (#1546), run headless with a mock process probe and mock
/// actions through its probe-factory seam: the table lists the probe's processes, the tree view nests
/// a child under its parent (and toggling it before the table's first frame no longer asserts, #1656),
/// and an inactive panel draws nothing. Selection raises application events, which go to the test's
/// event sink through the panel's second seam: keyboard selection announces the selected process. A
/// right-click opens the row menu for that process, whose Details, Copy PID and Kill (through its
/// confirmation, to the mock actions) work; the Columns menu shows a hidden column and the meter menu
/// turns a meter on. Ctrl+A selects every row, F9 then asks to kill them all in one confirmation, and
/// the row menu on a selection sets one priority for all of it through the batch dialog (#804, #1484).
/// The startup selection (TASKSMACK_SELECT, #1559) selects its process, by PID or by name, once it is
/// listed, and opens Details and its tab when asked to. onUpdate() gives the sampler the refresh setting,
/// slowed while the window is resized or moved and for a moment after, and relaxed on a tab with no
/// process data.

#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/SamplingConfig.h"
#include "Mocks/MockProbes.h"
#include "UI/IconsFontAwesome6.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace App
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;

class ProcessesPanelRenderTest : public ::testing::Test
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
        m_Settings = UserConfig::get().settings(); // the menus write the column and meter settings
    }

    void TearDown() override
    {
        UserConfig::get().settings() = m_Settings;
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// An attached panel reading three processes: init (1), server (100) and its child worker (101). The
    /// events it raises go to @p raiseEvent, when given; onUpdate() is told no interaction redraw is on.
    [[nodiscard]] static std::unique_ptr<ProcessesPanel> attachedPanel(std::function<void(Core::Event&)> raiseEvent = {})
    {
        auto panel = std::make_unique<ProcessesPanel>(
            []
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                    .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                    .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
                return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>()};
            },
            std::move(raiseEvent),
            [] { return false; });
        panel->onAttach();
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }

    /// As attachedPanel(), with mock actions that can do everything; @p actions is left pointing at them
    /// (the panel owns them).
    [[nodiscard]] static std::unique_ptr<ProcessesPanel> attachedPanelWithActions(std::function<void(Core::Event&)> raiseEvent,
                                                                                  TestMocks::MockProcessActions*& actions)
    {
        auto panel = std::make_unique<ProcessesPanel>(
            [&actions]
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                    .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                    .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
                auto mock = std::make_unique<TestMocks::MockProcessActions>();
                mock->setCapabilities(
                    {.canTerminate = true, .canKill = true, .canStop = true, .canContinue = true, .canSetPriority = true});
                actions = mock.get();
                return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock)};
            },
            std::move(raiseEvent));
        panel->onAttach();
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }

    /// The popup open on top, if any.
    [[nodiscard]] static ImGuiWindow* topPopup()
    {
        return GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
    }

    /// Right-clicks down the panel until a row's menu opens; the PID its header names, if one did. Another
    /// popup on the way (the filter box's own context menu) is closed again.
    [[nodiscard]] static std::optional<std::int32_t> openARowMenu(ProcessesPanel& panel)
    {
        static const std::regex PID_IN_HEADER(R"(\(PID (\d+)\))");
        ImGuiIO& io = ImGui::GetIO();
        for (int step = 0; step < 94; ++step) // 40 px down to 600 px, every 6 px
        {
            io.AddMousePosEvent(40.0F, 40.0F + (6.0F * static_cast<float>(step)));
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
            static_cast<void>(renderAndCapture(panel));
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
            const std::string text = renderAndCapture(panel);
            if (GImGui->OpenPopupStack.empty())
            {
                continue;
            }
            if (std::smatch match; std::regex_search(text, match, PID_IN_HEADER))
            {
                return std::stoi(match[1].str());
            }
            io.AddKeyEvent(ImGuiKey_Escape, true);
            static_cast<void>(renderAndCapture(panel));
            io.AddKeyEvent(ImGuiKey_Escape, false);
            static_cast<void>(renderAndCapture(panel));
        }
        return std::nullopt;
    }

    /// Presses the top popup's item labelled @p label, then draws the frame that acts on it.
    static void pressPopupItem(ProcessesPanel& panel, const std::string& label)
    {
        const ImGuiWindow* popup = topPopup();
        ASSERT_NE(popup, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, popup->ID));
        static_cast<void>(renderAndCapture(panel));
    }

    /// Ctrl+A over the panel: every row shown selected (#804).
    static void selectAll(ProcessesPanel& panel)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // hovered: navigation is armed
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        static_cast<void>(renderAndCapture(panel));
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        static_cast<void>(renderAndCapture(panel));
    }

    /// Presses the top-most modal's filled button labelled @p label (UI::Widgets::filledButton():
    /// "##filled" under the label's ID), then draws the frames that act on it.
    static void pressModalFilledButton(ProcessesPanel& panel, const std::string& label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr("##filled", 0, ImHashStr(label.c_str(), 0, modal->ID)));
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
    }

    /// One frame of a window filling the display, drawing @p panel's content, and the text it drew.
    [[nodiscard]] static std::string renderAndCapture(ProcessesPanel& panel)
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

    /// Renders until the panel has read all three processes (the sampler runs on its own thread).
    static void renderUntilLoaded(ProcessesPanel& panel)
    {
        for (int frame = 0; frame < LOAD_FRAMES && panel.processCount() < 3; ++frame)
        {
            static_cast<void>(renderAndCapture(panel));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        static_cast<void>(renderAndCapture(panel)); // the table draws the rows it now has
    }

    /// As renderUntilLoaded(), with onUpdate() before each frame as the app's loop runs it: it adopts the
    /// sampler's snapshots and applies the startup selection.
    static void updateUntilLoaded(ProcessesPanel& panel)
    {
        for (int frame = 0; frame < LOAD_FRAMES && panel.processCount() < 3; ++frame)
        {
            panel.onUpdate(1.0F / 60.0F);
            static_cast<void>(renderAndCapture(panel));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        panel.onUpdate(1.0F / 60.0F);
        static_cast<void>(renderAndCapture(panel));
    }

    /// Presses and releases @p key over the panel, one frame each.
    static void pressKey(ProcessesPanel& panel, ImGuiKey key)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // hovered: navigation is armed
        io.AddKeyEvent(key, true);
        static_cast<void>(renderAndCapture(panel));
        io.AddKeyEvent(key, false);
        static_cast<void>(renderAndCapture(panel));
    }

    static constexpr int LOAD_FRAMES = 200;

  private:
    UserSettings m_Settings;
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

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

/// The process ids of the ProcessSelectedEvents raised, in order.
struct SelectionSink
{
    std::vector<std::int32_t> selected;
    int showDetails = 0;

    [[nodiscard]] std::function<void(Core::Event&)> sink()
    {
        return [this](Core::Event& event)
        {
            if (const auto* selection = dynamic_cast<const Core::ProcessSelectedEvent*>(&event); selection != nullptr)
            {
                selected.push_back(selection->getPid());
            }
            else if (dynamic_cast<const Core::ShowProcessDetailsEvent*>(&event) != nullptr)
            {
                ++showDetails;
            }
        };
    }
};

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
    EXPECT_EQ(actions->killCount(), 1);
    EXPECT_EQ(actions->lastKillPid(), menuPid.value_or(-1));
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
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
    pressPopupItem(*panel, "CPU %");
    EXPECT_NE(UserConfig::get().settings().processMeters.isOn(ProcessColumn::CpuPercent), wasOn);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, CtrlAThenF9KillsEverySelectedProcessAfterOneConfirmation)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);

    selectAll(*panel);
    EXPECT_EQ(panel->selectionCount(), 3U);

    panel->requestKillSelected(); // F9, as ShellLayer hands it to the tab on show
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
    ASSERT_NE(modal, nullptr);
    EXPECT_TRUE(std::string_view(modal->Name).contains("3")) << modal->Name; // one dialog for all three
    EXPECT_EQ(actions->killCount(), 0);

    pressModalFilledButton(*panel, "Kill");
    EXPECT_EQ(actions->killCount(), 3); // each, by identity
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheRowMenuOnASelectionSetsOnePriorityForAllOfIt)
{
    TestMocks::MockProcessActions* actions = nullptr;
    const auto panel = attachedPanelWithActions([](Core::Event&) {}, actions);
    ASSERT_NE(actions, nullptr);
    renderUntilLoaded(*panel);
    selectAll(*panel);
    ASSERT_EQ(panel->selectionCount(), 3U);

    // A right-click on a row of the selection keeps it, and the menu acts on all of it.
    ASSERT_TRUE(openARowMenu(*panel).has_value());
    EXPECT_EQ(panel->selectionCount(), 3U);
    pressPopupItem(*panel, std::format("{} Set priority for 3 processes...###SetPriority", ICON_FA_GAUGE_HIGH));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr);
    EXPECT_EQ(actions->setPriorityCount(), 0);

    // The dialog lists them, so it is the confirmation (#1539): Apply sets the value on each.
    pressModalFilledButton(*panel, "Apply to 3 processes");
    EXPECT_EQ(actions->setPriorityCount(), 3);
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheStartupSelectionPicksItsProcessByPidAndOpensDetails)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    panel->requestStartupSelection(SelectOverride::Target{.pid = 101, .name = {}, .tab = SelectOverride::DetailsTab::Network}, true);
    EXPECT_FALSE(panel->takeStartupDetailsTab().has_value()); // nothing until it fires
    updateUntilLoaded(*panel);

    EXPECT_EQ(panel->selectedPid(), 101);
    EXPECT_EQ(panel->selectionCount(), 1U);
    ASSERT_FALSE(events.selected.empty());
    EXPECT_EQ(events.selected.back(), 101);
    EXPECT_EQ(events.showDetails, 1); // as the row menu's Details
    // The Details tab it asked for, handed over once (TASKSMACK_DETAILS_TAB).
    EXPECT_EQ(panel->takeStartupDetailsTab(), std::optional<SelectOverride::DetailsTab>(SelectOverride::DetailsTab::Network));
    EXPECT_FALSE(panel->takeStartupDetailsTab().has_value());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheStartupSelectionFindsAProcessByName)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    panel->requestStartupSelection(SelectOverride::Target{.pid = std::nullopt, .name = "server", .tab = {}}, false);
    updateUntilLoaded(*panel);

    EXPECT_EQ(panel->selectedPid(), 100);
    EXPECT_EQ(events.showDetails, 0); // not asked to open Details
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheSamplingIntervalFollowsInteractionAndTheTabOnShow)
{
    bool interacting = false;
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0));
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>()};
        },
        [](Core::Event&) {},
        [&interacting] { return interacting; });
    panel->onAttach();
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    const auto base = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
    const auto normal = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(base, true, false);
    const auto throttled = AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(base, true, true);
    ASSERT_GT(throttled, normal);

    panel->onUpdate(0.016F);
    EXPECT_EQ(panel->appliedSamplingInterval(), normal);

    // A resize or move slows sampling while it lasts...
    interacting = true;
    panel->onUpdate(0.016F);
    EXPECT_EQ(panel->appliedSamplingInterval(), throttled);

    // ...and for a moment after, so a drag that pauses doesn't flap the interval...
    interacting = false;
    panel->onUpdate(0.1F);
    EXPECT_EQ(panel->appliedSamplingInterval(), throttled);

    // ...then the setting comes back.
    panel->onUpdate(0.5F);
    EXPECT_EQ(panel->appliedSamplingInterval(), normal);

    // A tab that shows no process data relaxes it; coming back restores it.
    Core::ActiveTabChangedEvent away("SystemInfo");
    panel->onEvent(away);
    panel->onUpdate(0.016F);
    EXPECT_EQ(panel->appliedSamplingInterval(), AdaptiveIntervalUtils::chooseAdaptiveProcessInterval(base, false, false));
    EXPECT_GT(panel->appliedSamplingInterval(), normal);
    panel->onEvent(shown);
    panel->onUpdate(0.016F);
    EXPECT_EQ(panel->appliedSamplingInterval(), normal);
    panel->onDetach();
}

} // namespace
} // namespace App
