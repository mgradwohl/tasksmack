/// @file test_ProcessesPanelRender.cpp
/// @brief The Processes panel as a whole (#1546), run headless with a mock process probe and mock
/// actions through its probe-factory seam: the table lists the probe's processes, the tree view nests
/// a child under its parent (and toggling it before the table's first frame no longer asserts, #1656),
/// and an inactive panel draws nothing. Selection raises application events, which go to the test's
/// event sink through the panel's second seam: keyboard selection announces the selected process. A
/// right-click opens the row menu for that process, whose Details, Copy PID and Kill (through its
/// confirmation, to the mock actions) work; the Columns menu shows a hidden column and the meter menu
/// turns a meter on. Ctrl+A selects every row, F9 then asks to kill them all in one confirmation, and
/// the row menu on a selection sets one priority for all of it through the batch dialog (#804, #1484);
/// the primary row of a selection, the one Process Details shows, is marked (#1926).
/// The startup selection (TASKSMACK_SELECT, #1559) selects its process, by PID or by name, once it is
/// listed, and opens Details and its tab when asked to. onUpdate() gives the sampler the refresh setting,
/// slowed while the window is resized or moved and for a moment after, and relaxed on a tab with no
/// process data.

#include "App/Panels/AdaptiveIntervalUtils.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessesPanel.h"
#include "App/ProcessColumnConfig.h"
#include "App/SelectOverride.h"
#include "App/UserConfig.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Domain/GPUModel.h"
#include "Domain/ProcessModel.h"
#include "Domain/SamplingConfig.h"
#include "Domain/ServiceModel.h"
#include "Mocks/FakeClipboard.h"
#include "Mocks/MockGPUProbe.h"
#include "Mocks/MockProbes.h"
#include "Platform/GPUTypes.h"
#include "Platform/IServiceProbe.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <map>
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
        m_Clipboard.install(); // Copy PID: never the system clipboard, which other processes share (#1890)
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
    /// Its GPU model, which says whether per-process GPU data and an NPU are there, is @p gpuModel, or
    /// none.
    [[nodiscard]] static std::unique_ptr<ProcessesPanel> attachedPanel(std::function<void(Core::Event&)> raiseEvent = {},
                                                                       const std::shared_ptr<const Domain::GPUModel>& gpuModel = nullptr)
    {
        auto panel = std::make_unique<ProcessesPanel>(
            []
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                    .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                    .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
                return ProcessesPanelPlatform{
                    .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
            },
            std::move(raiseEvent),
            [] { return false; });
        panel->setGpuModel(gpuModel);
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
                return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
            },
            std::move(raiseEvent));
        panel->onAttach();
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }

    /// A GPU model with one GPU whose probe reports per-process metrics and utilization, and with
    /// @p withNpu, an NPU too (#1870). It publishes what it lists on its first refresh(), as the GPU
    /// sampler's first sample does; until then whether there is an NPU is unknown.
    [[nodiscard]] static std::shared_ptr<Domain::GPUModel> gpuModel(bool withNpu)
    {
        Platform::GPUCapabilities caps;
        caps.hasPerProcessMetrics = true;
        caps.hasPerProcessUtilization = true;
        auto probe = std::make_unique<TestMocks::MockGPUProbe>();
        probe->withGPU("gpu0", "Test GPU").withCapabilities(caps);
        if (withNpu)
        {
            probe->withAccelerator("npu0", "Intel(R) AI Boost", "0x00000000_0x0000abcd");
        }
        return std::make_shared<Domain::GPUModel>(std::move(probe));
    }

    /// Opens the toolbar's Columns menu.
    static void openColumnsMenu(ProcessesPanel& panel)
    {
        const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
        ASSERT_NE(shell, nullptr);
        ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
        ASSERT_NE(topPopup(), nullptr);
    }

    /// Closes every popup, so the next frame's text is the table's alone (the Columns menu lists every
    /// column); the table has any column it changed from the next frame.
    static void closePopups(ProcessesPanel& panel)
    {
        ImGui::ClosePopupToLevel(0, /*restore_focus_to_window_under_popup=*/true);
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
        ASSERT_EQ(topPopup(), nullptr);
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

    /// Clicks the left mouse button at height @p y over the panel, with Ctrl and/or Shift held, one
    /// frame down and one up.
    static void clickAt(ProcessesPanel& panel, float y, bool ctrl = false, bool shift = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddMousePosEvent(40.0F, y);
        // Two frames: the rows allow overlap, and an overlapping item only takes the hover once it was
        // already hovered the frame before, so a press on the first frame over a new row would miss it.
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderAndCapture(panel));
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        static_cast<void>(renderAndCapture(panel));
    }

    /// Each row's height on screen, by PID, found by plain clicks from the bottom up. Bottom-up so the
    /// scan meets every row before it could reach the header, whose click would re-sort the rows.
    [[nodiscard]] static std::map<std::int32_t, float> rowHeights(ProcessesPanel& panel, std::size_t rows)
    {
        std::map<std::int32_t, float> heights;
        for (int step = 0; step <= 140 && heights.size() < rows; ++step) // 600 px up to 40 px, every 4 px
        {
            const float y = 600.0F - (4.0F * static_cast<float>(step));
            clickAt(panel, y);
            if (const std::int32_t pid = panel.selectedPid(); pid > 0 && !heights.contains(pid))
            {
                heights.emplace(pid, y);
            }
        }
        return heights;
    }

    static constexpr int LOAD_FRAMES = 200;

  private:
    UserSettings m_Settings;
    TestMocks::FakeClipboard m_Clipboard;
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

// #1799: the Columns menu lists NPU %, NPU Memory and NPU Engine, all off by default. Shown with no
// NPU to read -- this panel has no GPU model, as Linux has no NPU data yet (#1827) -- the column's
// cells are the unavailable dash, as the GPU columns' are without per-process data.
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

// #1395: a text column this system can't fill (the mock probe reports no publisher, type, container or
// per-process GPU) reads as the unavailable dash in every row when the user has chosen to show it.
TEST_F(ProcessesPanelRenderTest, ChosenColumnsThisSystemCantFillShowTheDash)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col :
         {ProcessColumn::Publisher, ProcessColumn::Type, ProcessColumn::Container, ProcessColumn::GpuEngine, ProcessColumn::GpuDevice})
    {
        columns.setVisible(col, true);
    }
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Publisher")) << text;
    EXPECT_TRUE(text.contains("Container")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << text;
    panel->onDetach();
}

// #1880: the Services column, once a service list with PIDs is attached, lists each process's services
// comma-joined -- blank for one hosting none -- and sorts by that text.
TEST_F(ProcessesPanelRenderTest, TheServicesColumnListsEachProcesssServices)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Services, true);
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    // No service list yet: this system can't tell (the mock probe has no systemd units either).
    EXPECT_FALSE(panel->processCapabilities().hasServices);

    Platform::ServiceCapabilities listCaps;
    listCaps.canEnumerate = true;
    listCaps.hasPid = true;
    auto serviceProbe = std::make_unique<TestMocks::MockServiceProbe>(listCaps);
    Platform::ServiceInfo dhcp;
    dhcp.name = "Dhcp";
    dhcp.pid = 100;
    Platform::ServiceInfo eventLog;
    eventLog.name = "EventLog";
    eventLog.pid = 100;
    serviceProbe->setEnumeration(Platform::ServiceEnumeration::succeeded({eventLog, dhcp}));
    const auto services = std::make_shared<Domain::ServiceModel>(std::move(serviceProbe));
    services->sample();
    panel->processModel()->setServiceModel(services);
    panel->processModel()->refresh(); // the sampler's next refresh, now

    std::string text;
    int frame = 0;
    while (!text.contains("Dhcp, EventLog") && frame < LOAD_FRAMES)
    {
        text = renderAndCapture(*panel);
        ++frame;
    }
    EXPECT_TRUE(text.contains("Services")) << text;
    EXPECT_TRUE(text.contains("Dhcp, EventLog")) << text; // the list's name order
    EXPECT_TRUE(panel->processCapabilities().hasServices);
    panel->onDetach();
}

// #1395: "Reset columns" brings back this system's default columns and their default order.
TEST_F(ProcessesPanelRenderTest, ResetColumnsBringsBackTheDefaults)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Threads, true);
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderAndCapture(*panel).contains("Threads"));

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_TABLE_COLUMNS " Columns", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(topPopup(), nullptr);
    pressPopupItem(*panel, ICON_FA_ROTATE_LEFT " Reset columns");
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Threads")); // hidden by default again
    EXPECT_FALSE(UserConfig::get().settings().processColumns.isVisible(ProcessColumn::Threads));
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
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
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

TEST_F(ProcessesPanelRenderTest, PlainCtrlAndShiftClicksBuildTheSelection)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ASSERT_EQ(panel->processCount(), 3U);
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);
    // The rows top to bottom.
    std::vector<std::pair<float, std::int32_t>> byHeight;
    byHeight.reserve(heights.size());
    for (const auto& [pid, y] : heights)
    {
        byHeight.emplace_back(y, pid);
    }
    std::ranges::sort(byHeight);
    const auto [topY, top] = byHeight.front();
    const auto [middleY, middle] = byHeight[1];
    const auto [bottomY, bottom] = byHeight.back();

    // A plain click selects one row and announces it.
    clickAt(*panel, topY);
    EXPECT_EQ(panel->selectedPid(), top);
    EXPECT_EQ(panel->selectionCount(), 1U);
    ASSERT_FALSE(events.selected.empty());
    EXPECT_EQ(events.selected.back(), top);

    // Ctrl+click adds a row, which becomes the primary one.
    clickAt(*panel, middleY, /*ctrl=*/true);
    EXPECT_EQ(panel->selectionCount(), 2U);
    EXPECT_EQ(panel->selectedPid(), middle);

    // Ctrl+click on it again takes it out; the one left becomes the primary, so F9 still has a target.
    clickAt(*panel, middleY, /*ctrl=*/true);
    EXPECT_EQ(panel->selectionCount(), 1U);
    EXPECT_EQ(panel->selectedPid(), top);

    // Shift+click selects the range from the anchor down to the bottom row.
    clickAt(*panel, bottomY, /*ctrl=*/false, /*shift=*/true);
    EXPECT_GE(panel->selectionCount(), 2U);
    EXPECT_EQ(panel->selectedPid(), bottom);
    panel->onDetach();
}

/// The vertical extent of everything drawn in the primary row's marker colour (the theme stub's
/// selectionPrimary, used by nothing else) in the last frame, or nullopt if nothing was. Only opaque
/// vertices count, not the transparent fringe of anti-aliased strokes.
[[nodiscard]] std::optional<std::pair<float, float>> primaryMarkerExtent()
{
    const ImU32 marker = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().selectionPrimary);
    const ImDrawData* drawData = ImGui::GetDrawData();
    std::optional<std::pair<float, float>> extent;
    if (drawData == nullptr)
    {
        return extent;
    }
    for (const ImDrawList* list : drawData->CmdLists)
    {
        for (const ImDrawVert& vert : list->VtxBuffer)
        {
            if (vert.col != marker)
            {
                continue;
            }
            extent = extent.has_value() ? std::pair{std::min(extent->first, vert.pos.y), std::max(extent->second, vert.pos.y)}
                                        : std::pair{vert.pos.y, vert.pos.y};
        }
    }
    return extent;
}

// #1926: with several rows selected, the primary one -- the process Process Details shows -- is marked
// (a bar and an outline in the theme's selectionPrimary), and the other selected rows are not; a lone
// selected row is not marked. In the list and in the tree.
TEST_F(ProcessesPanelRenderTest, ThePrimaryRowOfASelectionIsMarked)
{
    for (const bool tree : {false, true})
    {
        SCOPED_TRACE(tree ? "tree view" : "list view");
        SelectionSink events;
        const auto panel = attachedPanel(events.sink());
        if (panel->treeViewEnabled() != tree)
        {
            panel->toggleTreeView();
        }
        renderUntilLoaded(*panel);
        ASSERT_EQ(panel->processCount(), 3U);
        const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
        ASSERT_EQ(heights.size(), 3U);
        std::vector<std::pair<float, std::int32_t>> byHeight;
        byHeight.reserve(heights.size());
        for (const auto& [pid, y] : heights)
        {
            byHeight.emplace_back(y, pid);
        }
        std::ranges::sort(byHeight);
        const float topY = byHeight[0].first;
        const float middleY = byHeight[1].first;
        const float bottomY = byHeight[2].first;
        const float rowPitch = (bottomY - topY) / 2.0F;

        // One row selected: it is the only candidate, so nothing marks it.
        clickAt(*panel, topY);
        ASSERT_EQ(panel->selectionCount(), 1U);
        EXPECT_FALSE(primaryMarkerExtent().has_value());

        // Two more Ctrl+clicked: the last one is the primary, and the marker is on its row alone.
        clickAt(*panel, middleY, /*ctrl=*/true);
        clickAt(*panel, bottomY, /*ctrl=*/true);
        ASSERT_EQ(panel->selectionCount(), 3U);
        ASSERT_EQ(panel->selectedPid(), byHeight[2].second);
        auto extent = primaryMarkerExtent();
        ASSERT_TRUE(extent.has_value());
        EXPECT_LE(extent->first, bottomY);
        EXPECT_GE(extent->second, bottomY);
        // One row tall, no more (the row pitch is only known to within the scan's 4 px, and the outline's
        // anti-aliased stroke reaches a pixel past the row).
        EXPECT_LT(extent->second - extent->first, 1.5F * rowPitch);
        for (const float other : {topY, middleY})
        {
            EXPECT_TRUE(other < extent->first + 1.0F || other > extent->second - 1.0F) << other;
        }

        // The top row Ctrl+clicked out and back in becomes the primary: the marker follows it.
        clickAt(*panel, topY, /*ctrl=*/true);
        clickAt(*panel, topY, /*ctrl=*/true);
        ASSERT_EQ(panel->selectionCount(), 3U);
        ASSERT_EQ(panel->selectedPid(), byHeight[0].second);
        extent = primaryMarkerExtent();
        ASSERT_TRUE(extent.has_value());
        EXPECT_LE(extent->first, topY);
        EXPECT_GE(extent->second, topY);
        for (const float other : {middleY, bottomY})
        {
            EXPECT_TRUE(other < extent->first + 1.0F || other > extent->second - 1.0F) << other;
        }
        panel->onDetach();
    }
}

TEST_F(ProcessesPanelRenderTest, LeavingTheTreeViewShowsTheSortedListAgain)
{
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const bool wasTree = panel->treeViewEnabled();
    panel->toggleTreeView();
    static_cast<void>(renderAndCapture(*panel));
    panel->toggleTreeView(); // back: the list is sorted again (#1174)
    EXPECT_EQ(panel->treeViewEnabled(), wasTree);
    static_cast<void>(renderAndCapture(*panel));
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, ArrowKeysWalkAndFoldTheTree)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    // The tree: init (1) > server (100) > worker (101).
    pressKey(*panel, ImGuiKey_Home);
    EXPECT_EQ(panel->selectedPid(), 1);
    pressKey(*panel, ImGuiKey_RightArrow); // expanded already: to its first child
    EXPECT_EQ(panel->selectedPid(), 100);
    pressKey(*panel, ImGuiKey_RightArrow);
    EXPECT_EQ(panel->selectedPid(), 101);
    pressKey(*panel, ImGuiKey_LeftArrow); // a leaf: to its parent
    EXPECT_EQ(panel->selectedPid(), 100);

    pressKey(*panel, ImGuiKey_LeftArrow); // expanded: collapses it, the selection stays
    EXPECT_EQ(panel->selectedPid(), 100);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("worker"));

    pressKey(*panel, ImGuiKey_RightArrow); // collapsed: expands it
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(renderAndCapture(*panel).contains("worker"));
    EXPECT_EQ(panel->selectedPid(), 100);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, PageKeysJumpAPageAndStopAtTheEnds)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    pressKey(*panel, ImGuiKey_Home);
    const std::int32_t first = panel->selectedPid();
    pressKey(*panel, ImGuiKey_End);
    const std::int32_t last = panel->selectedPid();
    ASSERT_NE(first, last);
    // Three rows fit in a page: Page Up from the last row lands on the first, Page Down goes back.
    pressKey(*panel, ImGuiKey_PageUp);
    EXPECT_EQ(panel->selectedPid(), first);
    pressKey(*panel, ImGuiKey_PageDown);
    EXPECT_EQ(panel->selectedPid(), last);
    pressKey(*panel, ImGuiKey_PageDown); // already at the end: stays
    EXPECT_EQ(panel->selectedPid(), last);
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheFilterKeepsMatchingRowsAndClearingItBringsThemBack)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    // Click into the filter and type, a character a frame as a keyboard would.
    ImGui::ActivateItemByID(ImHashStr("##search", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    for (const char* character : {"w", "o", "r", "k"})
    {
        ImGui::GetIO().AddInputCharactersUTF8(character);
        static_cast<void>(renderAndCapture(*panel));
    }
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("server")) << text; // only the matching row is left

    // The clear button beside the filter brings every row back. Enter first: a click on the button
    // takes the keyboard from the filter, which an active text field would otherwise write back over.
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    static_cast<void>(renderAndCapture(*panel));
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_XMARK, 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, HoldingCtrlOverTheTablePausesItAndSaysSo)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(DISPLAY_WIDTH * 0.5F, DISPLAY_HEIGHT * 0.5F); // the pane hovered
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    // The freeze engages once Ctrl has been held a moment (ProcessDisplayFreeze::ENGAGE_DELAY_SECONDS).
    std::string text;
    for (int i = 0; i < 30 && !text.contains("Paused (Ctrl)"); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        text = renderAndCapture(*panel);
    }
    EXPECT_TRUE(text.contains("Paused (Ctrl)")) << text;
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(renderAndCapture(*panel).contains("Paused (Ctrl)"));
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, TheListAndTreeSegmentsSwitchTheView)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_SITEMAP " Tree", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(panel->treeViewEnabled());
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_LIST " List", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_FALSE(panel->treeViewEnabled());
    panel->onDetach();
}

TEST_F(ProcessesPanelRenderTest, AColumnHeaderExplainsItsColumnOnHover)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    // The PID column's description, as its header's tooltip shows it (#1210).
    const std::string description(getColumnInfo(ProcessColumn::PID).description);
    ASSERT_FALSE(description.empty());
    ImGuiIO& io = ImGui::GetIO();
    // Down the header row's band, across the columns, holding still long enough for the tooltip.
    bool shown = false;
    for (int y = 20; y <= 140 && !shown; y += 6)
    {
        for (int x = 20; x <= 760 && !shown; x += 30)
        {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int frame = 0; frame < 30 && !shown; ++frame) // past ImGuiHoveredFlags_DelayShort at 60 fps
            {
                shown = renderAndCapture(*panel).contains(description);
            }
        }
    }
    EXPECT_TRUE(shown);
    panel->onDetach();
}

} // namespace
} // namespace App
