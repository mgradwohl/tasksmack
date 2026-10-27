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
/// listed, and opens Details and its tab when asked to. The sampler runs at the refresh setting in every
/// UI state (#2731): tab changes and the frame rate never change it.

#include "App/Panels/AuthorizeWording.h"
#include "App/Panels/ProcessColumnAvailability.h"
#include "App/Panels/ProcessDumpView.h"
#include "App/Panels/ProcessRowFormat.h"
#include "App/Panels/ProcessRowMeter.h"
#include "App/Panels/ProcessTreeIndent.h"
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
#include "Platform/CpuAffinity.h"
#include "Platform/GPUTypes.h"
#include "Platform/IProcessActions.h"
#include "Platform/IServiceProbe.h"
#include "Platform/ProcessTypes.h"
#include "UI/IconsFontAwesome6.h"
#include "UI/Theme.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <sstream>
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

/// onAttach(), then the seed the sampler thread takes (#2731), adopted as the first frame after it
/// would: the tests below read the rows, actions and capabilities as they stand after it.
void attachAndSeed(ProcessesPanel& panel)
{
    panel.onAttach();
    ASSERT_TRUE(panel.waitForFirstSample()) << "the sampler thread did not build the platform and seed the model in time";
}

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
    /// events it raises go to @p raiseEvent, when given. Its GPU model, which says whether per-process GPU
    /// data and an NPU are there, is @p gpuModel, or none.
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
            std::move(raiseEvent));
        panel->setGpuModel(gpuModel);
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }

    /// As attachedPanel(), with mock actions that can do everything; @p actions is left pointing at them
    /// (the panel owns them). onUpdate() is told no interaction redraw is on.
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
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
    }

    /// A GPU model with one GPU whose probe reports per-process metrics and utilization, and with
    /// @p withNpu, an NPU and per-process NPU data too (#1870, #1827). It publishes what it lists on its first refresh(), as the GPU
    /// sampler's first sample does; until then whether there is an NPU is unknown.
    [[nodiscard]] static std::shared_ptr<Domain::GPUModel> gpuModel(bool withNpu)
    {
        Platform::GPUCapabilities caps;
        caps.hasPerProcessMetrics = true;
        caps.hasPerProcessUtilization = true;
        // Per-process NPU data, apart from the GPU's (#1827), with the NPU.
        caps.hasPerProcessNpuMetrics = withNpu;
        caps.hasPerProcessNpuUtilization = withNpu;
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

    /// Presses the top-most modal's plain button labelled @p label (a non-destructive confirmation's:
    /// UI::Widgets::dialogFooter() draws it with ImGui::Button()), then draws the frames that act on it.
    static void pressModalButton(ProcessesPanel& panel, const std::string& label)
    {
        const ImGuiWindow* modal = ImGui::GetTopMostPopupModal();
        ASSERT_NE(modal, nullptr);
        ImGui::ActivateItemByID(ImHashStr(label.c_str(), 0, modal->ID));
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

    /// Draws frames, with onUpdate() before each as the app's loop runs it, until the toolbar shows
    /// @p result: a confirmed batch runs on its worker and onUpdate() takes its line in (#2142). Once it
    /// is shown the worker has finished (the panel took its result through the future), so its calls on
    /// the mock can be counted. Bounded by a deadline, not a frame count, so a slow worker (ASan, a
    /// loaded runner) is waited for and a broken one fails the test rather than hanging it.
    [[nodiscard]] static bool updateUntilBatchResult(ProcessesPanel& panel, std::string_view result)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            panel.onUpdate(1.0F / 60.0F);
            if (renderAndCapture(panel).contains(result))
            {
                return true;
            }
            std::this_thread::yield(); // let the worker run; the next frame polls again
        }
        return false;
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

    /// An attached panel reading @p processes, rendered until it has read them all, in tree view.
    [[nodiscard]] static std::unique_ptr<ProcessesPanel> loadedTreePanel(std::vector<Platform::ProcessCounters> processes)
    {
        const std::size_t count = processes.size();
        auto panel = std::make_unique<ProcessesPanel>(
            [processes = std::move(processes)]
            {
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                for (const Platform::ProcessCounters& process : processes)
                {
                    probe->withProcess(process);
                }
                return ProcessesPanelPlatform{
                    .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
            },
            [](Core::Event& /*event*/) {}); // selection events, which these tests do not read
        attachAndSeed(*panel);
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        for (int frame = 0; frame < LOAD_FRAMES && panel->processCount() < count; ++frame)
        {
            static_cast<void>(renderAndCapture(*panel));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!panel->treeViewEnabled())
        {
            panel->toggleTreeView();
        }
        static_cast<void>(renderAndCapture(*panel));
        static_cast<void>(renderAndCapture(*panel));
        return panel;
    }

    /// The Processes table: the one table in the context with a column for every ProcessColumn.
    [[nodiscard]] static const ImGuiTable* processTable()
    {
        for (int n = 0; n < GImGui->Tables.GetMapSize(); ++n)
        {
            const ImGuiTable* table = GImGui->Tables.TryGetMapData(n);
            if (table != nullptr && table->ColumnsCount == static_cast<int>(ProcessColumn::Count))
            {
                return table;
            }
        }
        return nullptr;
    }

    /// Clicks the left mouse button at (@p x, @p y), with the frames a click over a table needs.
    static void clickAtPoint(ProcessesPanel& panel, float x, float y)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMousePosEvent(x, y);
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        static_cast<void>(renderAndCapture(panel));
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        static_cast<void>(renderAndCapture(panel));
        static_cast<void>(renderAndCapture(panel));
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

// #1891: the row menu's Copy Name and Copy Command Line copy that process's, to the panel's clipboard.
TEST_F(ProcessesPanelRenderTest, CopyNameAndCopyCommandLinePutThemOnTheClipboard)
{
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                                  TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                                  TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100)})
            {
                counters.command = "/usr/bin/" + counters.name + " --flag";
                probe->withProcess(counters);
            }
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
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
    auto panel = std::make_unique<ProcessesPanel>(
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
        for (int step = 0; static_cast<float>(step) * 8.0F < DISPLAY_WIDTH && !shown; ++step)
        {
            ImGui::GetIO().AddMousePosEvent(DISPLAY_WIDTH - 2.0F - (static_cast<float>(step) * 8.0F), y);
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
    // Every process may run on CPU 0 only, and the actions can set an affinity (#1887).
    TestMocks::MockProcessActions* actions = nullptr;
    auto panel = std::make_unique<ProcessesPanel>(
        [&actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                                  TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                                  TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100)})
            {
                counters.cpuAffinity = Platform::CpuAffinity::fromMask(0b1U);
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canSetAffinity = true});
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
    static_cast<void>(renderAndCapture(*panel));
    ASSERT_NE(ImGui::GetTopMostPopupModal(), nullptr); // the dialog, with the process's own CPU ticked
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
    auto panel = std::make_unique<ProcessesPanel>(
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

// #1395: a text column this system can't fill (the mock probe reports no publisher, type, container or
// per-process GPU) reads as the unavailable dash in every row when the user has chosen to show it.
TEST_F(ProcessesPanelRenderTest, ChosenColumnsThisSystemCantFillShowTheDash)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : {ProcessColumn::Publisher,
                                    ProcessColumn::Type,
                                    ProcessColumn::Architecture,
                                    ProcessColumn::Container,
                                    ProcessColumn::GpuEngine,
                                    ProcessColumn::GpuDevice})
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

// #1891: where the probe classifies processes (Windows), the Type column shows each one's type in its
// colour, and the dash for a process it couldn't classify.
TEST_F(ProcessesPanelRenderTest, TheTypeColumnShowsEachProcesssTypeOrTheDash)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Type, true);
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters app = TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            app.processType = "Background Process";
            probe->withProcess(std::move(app))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasProcessType = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Background Process")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "worker isn't classified:\n" << text;
    panel->onDetach();
}

// #2394: the Architecture column shows the architecture each process runs as, and the dash for one whose
// architecture couldn't be read.
TEST_F(ProcessesPanelRenderTest, TheArchitectureColumnShowsEachProcesssArchitectureOrTheDash)
{
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Architecture, true);
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            Platform::ProcessCounters emulated =
                TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            emulated.architecture = "x64 (emulated)";
            probe->withProcess(std::move(emulated))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            Platform::ProcessCapabilities caps = probe->capabilities();
            caps.hasArchitecture = true;
            probe->setCapabilities(caps);
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
        },
        std::function<void(Core::Event&)>{});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Architecture")) << text;
    EXPECT_TRUE(text.contains("x64 (emulated)")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << "worker's isn't read:\n" << text;
    panel->onDetach();
    UserConfig::get().settings().processColumns.setVisible(ProcessColumn::Architecture, false);
}

// #1879: where the probe fills neither (Windows; the mock reports neither), the Private and Swap columns
// read as the dash in every row, and their headers and cells say why.
TEST_F(ProcessesPanelRenderTest, PrivateAndSwapWithoutTheirFiguresShowTheDashAndSayWhy)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    for (const ProcessColumn col : allProcessColumns())
    {
        if (getColumnInfo(col).canHide)
        {
            // PID, Name, Private and Swap: both headers are in reach
            columns.setVisible(col, col == ProcessColumn::Private || col == ProcessColumn::Swap);
        }
    }
    const auto panel = attachedPanel();
    renderUntilLoaded(*panel);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("Private")) << text;
    EXPECT_TRUE(text.contains("Swap")) << text;
    EXPECT_TRUE(text.contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT)) << text;

    // Over each point down the header and the first rows, across the whole width (the last column
    // stretches, and its right-aligned dash sits at the far end). Tooltips show without a hover delay,
    // so a few frames at each point are enough.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    style.HoverStationaryDelay = 0.0F;
    const auto hoverUntilShown = [&panel](std::string_view wanted)
    {
        ImGuiIO& io = ImGui::GetIO();
        for (int y = 20; y <= 140; y += 6)
        {
            for (int x = 10; x <= static_cast<int>(DISPLAY_WIDTH); x += 6)
            {
                io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
                for (int frame = 0; frame < 3; ++frame)
                {
                    if (renderAndCapture(*panel).contains(wanted))
                    {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    EXPECT_TRUE(hoverUntilShown(ProcessColumnAvailability::PRIVATE_UNSUPPORTED_COLUMN_NOTE)) << "Private's header tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessRowFormat::PRIVATE_UNSUPPORTED_CELL_REASON)) << "a Private cell's tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessColumnAvailability::SWAP_UNSUPPORTED_COLUMN_NOTE)) << "Swap's header tooltip";
    EXPECT_TRUE(hoverUntilShown(ProcessRowFormat::SWAP_UNSUPPORTED_CELL_REASON)) << "a Swap cell's tooltip";
    panel->onDetach();
}

// #1891: with per-process GPU data supported but not read yet for a process (it was first listed after
// the GPU sampler's last read), the GPU Engine and GPU Device cells read as the dash, and hovering one
// says why -- not the generic "couldn't be read" an empty list would get.
TEST_F(ProcessesPanelRenderTest, GpuTextCellsNotReadYetSayWhyOnHover)
{
    ProcessColumnSettings& columns = UserConfig::get().settings().processColumns;
    columns.setVisible(ProcessColumn::GpuEngine, true);
    columns.setVisible(ProcessColumn::GpuDevice, true);
    // The GPU sampler reads before the panel lists any process, so every process is listed after the
    // read: it may have started since, so its GPU fields are unread (#1210).
    const std::shared_ptr<Domain::GPUModel> gpu = gpuModel(false);
    gpu->refresh();
    std::this_thread::sleep_for(std::chrono::milliseconds(5)); // listed strictly after the read
    const auto panel = attachedPanel({}, gpu);
    panel->processModel()->setGPUModel(gpu); // as ShellLayer wires both
    panel->processModel()->refresh();
    renderUntilLoaded(*panel);
    ASSERT_TRUE(renderAndCapture(*panel).contains(ProcessRowFormat::UNAVAILABLE_CELL_TEXT));

    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    style.HoverStationaryDelay = 0.0F;
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    for (int y = 20; y <= 140 && !shown; y += 6)
    {
        for (int x = 10; x <= static_cast<int>(DISPLAY_WIDTH) && !shown; x += 6)
        {
            io.AddMousePosEvent(static_cast<float>(x), static_cast<float>(y));
            for (int frame = 0; frame < 3 && !shown; ++frame)
            {
                shown = renderAndCapture(*panel).contains(ProcessRowFormat::GPU_NOT_READ_YET_CELL_REASON);
            }
        }
    }
    EXPECT_TRUE(shown) << "a GPU text cell's not-read-yet reason";
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
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

    // Hovering the cell lists them one per line under a count (#1891); the tooltip waits for the mouse
    // to rest, so the hover delays are zeroed and the rows swept.
    ImGuiStyle& style = ImGui::GetStyle();
    style.HoverStationaryDelay = 0.0F;
    style.HoverDelayShort = 0.0F;
    style.HoverDelayNormal = 0.0F;
    const std::string heading = ProcessRowFormat::servicesTooltipHeading(2);
    // The Services column's cells, from the table ImGui laid out: sweep down that column only, so the
    // row's position (it follows the sort) doesn't matter, and few frames are drawn (a slow ASan run).
    const ImGuiTable* table = nullptr;
    int servicesColumn = -1;
    for (int t = 0; t < GImGui->Tables.GetMapSize() && servicesColumn < 0; ++t)
    {
        const ImGuiTable* candidate = GImGui->Tables.TryGetMapData(t);
        for (int c = 0; candidate != nullptr && c < candidate->ColumnsCount; ++c)
        {
            if (std::string_view(ImGui::TableGetColumnName(candidate, c)) == "Services")
            {
                table = candidate;
                servicesColumn = c;
                break;
            }
        }
    }
    ASSERT_NE(table, nullptr) << "no table with a Services column";
    const ImGuiTableColumn& column = table->Columns[servicesColumn];
    const float x = (column.MinX + column.MaxX) * 0.5F;
    bool shown = false;
    ImGuiIO& io = ImGui::GetIO();
    const int steps = static_cast<int>(table->InnerRect.GetHeight() / 4.0F);
    for (int step = 0; step < steps && !shown; ++step)
    {
        io.AddMousePosEvent(x, table->InnerRect.Min.y + 2.0F + (static_cast<float>(step) * 4.0F));
        for (int settle = 0; settle < 3 && !shown; ++settle)
        {
            const std::string hovered = renderAndCapture(*panel);
            shown = hovered.contains(heading) && hovered.contains("\nDhcp") && hovered.contains("\nEventLog");
        }
    }
    EXPECT_TRUE(shown) << "the Services cell's tooltip: \"" << heading << "\", then each name";
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
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    // On the batch's worker (#2142): the toolbar's line comes on a later frame.
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Kill sent to 3 processes"));
    EXPECT_EQ(actions->killCount(), 3); // each, by identity
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
    EXPECT_EQ(ImGui::GetTopMostPopupModal(), nullptr);
    ASSERT_TRUE(updateUntilBatchResult(*panel, "Priority set to")); // from the batch's worker (#2142)
    EXPECT_EQ(actions->setPriorityCount(), 3);
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

// #2731: the sampler runs at the refresh setting in every UI state. Settings is the one thing that sets
// it. A tab change (to a tab with process data or without), frames at any rate, and the slow frames of
// a minimized window change nothing. Nor can a resize or move: onUpdate() no longer asks whether one is
// on, so there is no state through which it could.
TEST_F(ProcessesPanelRenderTest, TheSamplerIntervalIsTheRefreshSettingInEveryUiState)
{
    const auto panel = attachedPanel();
    const auto setting = std::chrono::milliseconds(Domain::Sampling::REFRESH_INTERVAL_DEFAULT_MS);
    EXPECT_EQ(panel->samplingInterval(), setting);

    const auto expectThroughEveryUiState = [&panel](std::chrono::milliseconds expected)
    {
        // Each main tab shown in turn, with a frame on each: tabs with process data and tabs without.
        for (const char* tab : {"Processes", "SystemInfo", "SystemOverview", "Services", "ProcessDetails", "Startup", "Processes"})
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

// #838: tree view sorts each parent's children by the clicked column, and every child stays right
// under its parent; the list sorts the same processes flat.
TEST_F(ProcessesPanelRenderTest, TheTreeViewSortsSiblingsUnderTheirParents)
{
    using TestMocks::makeProcessCounters;
    constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
    const auto panel = loadedTreePanel({
        makeProcessCounters(1, "tree-root", 'S', 10, 5, 100, RSS, 0),
        makeProcessCounters(300, "alpha", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(200, "charlie", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(100, "bravo", 'S', 10, 5, 1000, RSS, 1),
        makeProcessCounters(400, "zulu", 'S', 10, 5, 2000, RSS, 300),
    });
    ASSERT_EQ(panel->processCount(), 5U);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    clickAtPoint(*panel, (name.MinX + name.MaxX) * 0.5F, table->OuterRect.Min.y + (ImGui::GetFrameHeight() * 0.5F));

    std::string text = renderAndCapture(*panel);
    const auto at = [&text](std::string_view label)
    {
        return text.find(label);
    };
    ASSERT_NE(at("zulu"), std::string::npos) << text;
    // tree-root, then its children by name, zulu under alpha though it sorts last by name.
    EXPECT_LT(at("tree-root"), at("alpha")) << text;
    EXPECT_LT(at("alpha"), at("zulu")) << text;
    EXPECT_LT(at("zulu"), at("bravo")) << text;
    EXPECT_LT(at("bravo"), at("charlie")) << text;

    panel->toggleTreeView(); // the list: the same sort, flat
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_LT(at("alpha"), at("bravo")) << text;
    EXPECT_LT(at("bravo"), at("charlie")) << text;
    EXPECT_LT(at("charlie"), at("tree-root")) << text;
    EXPECT_LT(at("tree-root"), at("zulu")) << text;
    panel->onDetach();
}

// #838 audit (#906, #911): the expander is drawn in the Name column, so a click at the start of a
// parent's Name cell folds it, while a click in its PID cell only selects the row.
TEST_F(ProcessesPanelRenderTest, TheTreeExpanderSitsInTheNameColumn)
{
    SelectionSink events; // a click selects a row, which raises an event
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& pid = table->Columns[static_cast<int>(toIndex(ProcessColumn::PID))];
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    ASSERT_LT(pid.MaxX, name.MinX + 1.0F) << "PID before Name, as by default";

    // Every row is found by clicks in the PID column, none of which folds anything.
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);
    EXPECT_TRUE(renderAndCapture(*panel).contains("worker"));

    // The root's expander: the start of its Name cell (no indent at depth 0), a text line high. The
    // height found for the row is the lowest point in it, so half a line up is about its middle.
    const float rootMiddle = heights.at(1) - (ImGui::GetTextLineHeight() * 0.5F);
    clickAtPoint(*panel, name.MinX + ImGui::GetStyle().CellPadding.x + (ImGui::GetFontSize() * 0.5F), rootMiddle);
    const std::string text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
    EXPECT_FALSE(text.contains("worker")) << text;

    // #2713: a second click on the expander opens the branch again.
    clickAtPoint(*panel, name.MinX + ImGui::GetStyle().CellPadding.x + (ImGui::GetFontSize() * 0.5F), rootMiddle);
    const std::string reopened = renderAndCapture(*panel);
    EXPECT_TRUE(reopened.contains("server")) << reopened;
    EXPECT_TRUE(reopened.contains("worker")) << reopened;
    panel->onDetach();
}

// #2713 (a #2700 manual check): a column dragged to a new place and one widened by its border, both with
// the mouse through the panel, come back the same once the layout has gone through config.toml into a
// fresh panel and a fresh ImGui context -- the sort with them.
TEST_F(ProcessesPanelRenderTest, ColumnsReorderedAndResizedByMouseSurviveARestart)
{
    // Frames in a window that keeps settings, as the app's does: a table in a NoSavedSettings window (the
    // other tests' "Shell") saves none.
    const auto frame = [](ProcessesPanel& shown)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("SavingShell", nullptr, ImGuiWindowFlags_NoDecoration);
        shown.renderContent();
        ImGui::End();
        ImGui::Render();
    };
    // Until it has read all three processes (the sampler runs on its own thread), then a frame to draw them.
    const auto load = [&frame](ProcessesPanel& shown)
    {
        for (int drawn = 0; drawn < LOAD_FRAMES && shown.processCount() < 3; ++drawn)
        {
            frame(shown);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        frame(shown);
        frame(shown);
    };
    auto panel = attachedPanel();
    load(*panel);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const auto pidIndex = static_cast<int>(toIndex(ProcessColumn::PID));
    const auto nameIndex = static_cast<int>(toIndex(ProcessColumn::Name));
    ASSERT_LT(table->Columns[pidIndex].DisplayOrder, table->Columns[nameIndex].DisplayOrder) << "PID before Name, as by default";
    const float headerY = table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + (ImGui::GetTextLineHeight() * 0.5F);
    ImGuiIO& io = ImGui::GetIO();
    // Presses at (@p fromX, headerY), moves to @p toX in steps, and releases, a frame or two each.
    const auto dragHeader = [&](float fromX, float toX)
    {
        io.AddMousePosEvent(fromX, headerY);
        frame(*panel);
        frame(*panel);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame(*panel);
        for (int step = 1; step <= 10; ++step)
        {
            io.AddMousePosEvent(fromX + ((toX - fromX) * static_cast<float>(step) / 10.0F), headerY);
            frame(*panel);
        }
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame(*panel);
        frame(*panel);
    };

    // Name's header dragged onto PID's: Name goes first.
    const ImGuiTableColumn& pid = table->Columns[pidIndex];
    const ImGuiTableColumn& name = table->Columns[nameIndex];
    dragHeader((name.MinX + name.MaxX) * 0.5F, pid.MinX + 2.0F);
    ASSERT_LT(name.DisplayOrder, pid.DisplayOrder) << "the drag reordered them";

    // PID's right border dragged 40 px right: wider.
    const float widthBefore = pid.WidthGiven;
    dragHeader(pid.MaxX - 1.0F, pid.MaxX + 39.0F);
    EXPECT_GT(pid.WidthGiven, widthBefore + 20.0F) << "the border drag widened it";
    const int nameOrder = name.DisplayOrder;
    const int pidOrder = pid.DisplayOrder;
    const float pidWidth = pid.WidthGiven;

    const std::string layout = panel->captureTableLayout();
    ASSERT_FALSE(layout.empty());
    panel->onDetach();
    panel.reset();

    // Through config.toml, as the writer writes it and load() reads it.
    std::ostringstream file;
    file << toml::table{{"process_table", toml::table{{"layout", layout}}}};
    UserSettings loaded;
    ASSERT_TRUE(UserConfig::parseSettings(file.str(), loaded));
    ASSERT_EQ(loaded.processTableLayout, layout);

    // A fresh context remembers nothing of the table; only the stored layout can put it back.
    ImGuiContext* original = ImGui::GetCurrentContext();
    ImPlotContext* originalPlot = ImPlot::GetCurrentContext();
    ImGuiContext* fresh = ImGui::CreateContext();
    ImGui::SetCurrentContext(fresh);
    ImPlotContext* freshPlot = ImPlot::CreateContext();
    ImGuiIO& freshIo = ImGui::GetIO();
    freshIo.IniFilename = nullptr;
    freshIo.LogFilename = nullptr;
    freshIo.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    freshIo.DeltaTime = 1.0F / 60.0F;
    freshIo.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    freshIo.Fonts->AddFontDefault();
    ProcessesPanel::restoreTableLayout(loaded.processTableLayout);
    auto restarted = attachedPanel();
    load(*restarted);
    const ImGuiTable* restored = processTable();
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->Columns[nameIndex].DisplayOrder, nameOrder);
    EXPECT_EQ(restored->Columns[pidIndex].DisplayOrder, pidOrder);
    EXPECT_NEAR(restored->Columns[pidIndex].WidthGiven, pidWidth, 1.0F);
    EXPECT_EQ(restarted->captureTableLayout(), layout) << "the same layout, its sort included";
    restarted->onDetach();
    restarted.reset();
    ImPlot::DestroyContext(freshPlot);
    ImGui::DestroyContext(fresh);
    ImGui::SetCurrentContext(original);
    ImPlot::SetCurrentContext(originalPlot);
}

// #838 audit (#852, #1138): the tree view goes through ImGuiListClipper, so only the rows in view
// submit draw calls: a frame of 2000 processes draws about what a frame of 100 does, at the top and
// after End scrolls to the bottom. Measured in vertices, not logged text: ImGui turns clipping off
// while logging.
TEST_F(ProcessesPanelRenderTest, TheTreeViewDrawsOnlyTheRowsInView)
{
    // A root and @p children processes under it.
    const auto tree = [](int children)
    {
        constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
        std::vector<Platform::ProcessCounters> processes;
        processes.reserve(static_cast<std::size_t>(children) + 1);
        processes.push_back(TestMocks::makeProcessCounters(1, "tree-root", 'S', 10, 5, 100, RSS, 0));
        for (int i = 0; i < children; ++i)
        {
            const int pid = 1000 + i;
            processes.push_back(TestMocks::makeProcessCounters(pid, std::format("proc-{}", pid), 'S', 10, 5, 1000, RSS, 1));
        }
        return processes;
    };
    // The vertices one frame of @p panel draws, without logging.
    const auto frameVertices = [](ProcessesPanel& panel)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Shell", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        panel.renderContent();
        ImGui::End();
        ImGui::Render();
        return ImGui::GetIO().MetricsRenderVertices;
    };

    // 100 children already overflow the 1200 px table, so its frame is a screenful of rows.
    const auto screenful = loadedTreePanel(tree(99));
    ASSERT_EQ(screenful->processCount(), 100U);
    const int screenfulVertices = frameVertices(*screenful);
    screenful->onDetach();

    const auto panel = loadedTreePanel(tree(1999));
    ASSERT_EQ(panel->processCount(), 2000U);
    const int atTop = frameVertices(*panel);
    EXPECT_GT(atTop, 0);
    EXPECT_LT(atTop, screenfulVertices * 3 / 2) << "20 times the rows, about the same drawing";

    pressKey(*panel, ImGuiKey_End); // the last row, scrolled into view
    static_cast<void>(frameVertices(*panel));
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    EXPECT_GT(table->InnerWindow->Scroll.y, 0.0F);
    EXPECT_FLOAT_EQ(table->InnerWindow->Scroll.y, table->InnerWindow->ScrollMax.y);
    EXPECT_LT(frameVertices(*panel), screenfulVertices * 3 / 2);
    panel->onDetach();
}

// #838: tree view's Collapse all folds every parent, leaving the roots, and Expand all opens them again.
TEST_F(ProcessesPanelRenderTest, TheTreeViewsCollapseAllAndExpandAllFoldEveryParent)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("worker")) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_UP "##CollapseAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("init")) << text;
    EXPECT_FALSE(text.contains("server")) << text;
    EXPECT_FALSE(text.contains("worker")) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_DOWN "##ExpandAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("server")) << text;
    EXPECT_TRUE(text.contains("worker")) << text;
    panel->onDetach();
}

// #838: a filtered tree keeps a match under its ancestors, which do not match and are drawn dimmed,
// rather than promoting the match to a root; the list still shows the match alone.
TEST_F(ProcessesPanelRenderTest, AFilteredTreeShowsTheMatchUnderItsDimmedAncestors)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    // Where each row is, found before filtering: the filtered tree has the same rows in the same places.
    const std::map<std::int32_t, float> heights = rowHeights(*panel, 3);
    ASSERT_EQ(heights.size(), 3U);

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
    ASSERT_TRUE(text.contains("worker")) << text;
    EXPECT_LT(text.find("init"), text.find("server")) << text;
    EXPECT_LT(text.find("server"), text.find("worker")) << text;

    // The brightest vertex in each row's Name cell: the ancestors' are about half the match's.
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    const ImDrawList* drawList = table->InnerWindow->DrawList;
    const float rowHeight = heights.at(101) - heights.at(100);
    ASSERT_GT(rowHeight, 0.0F);
    const auto brightest = [&](std::int32_t pid)
    {
        const float bottom = heights.at(pid);
        unsigned int alpha = 0;
        for (const ImDrawVert& vert : drawList->VtxBuffer)
        {
            if (vert.pos.x >= name.MinX && vert.pos.x <= name.MaxX && vert.pos.y > bottom - rowHeight + 4.0F && vert.pos.y <= bottom)
            {
                alpha = std::max(alpha, (vert.col >> IM_COL32_A_SHIFT) & 0xFFU);
            }
        }
        return alpha;
    };
    const unsigned int match = brightest(101);
    EXPECT_GT(match, 200U);
    EXPECT_LT(brightest(1), match * 3 / 4);
    EXPECT_GT(brightest(1), 0U);
    EXPECT_LT(brightest(100), match * 3 / 4);

    panel->toggleTreeView(); // the list: the match alone
    static_cast<void>(renderAndCapture(*panel));
    const std::string list = renderAndCapture(*panel);
    EXPECT_TRUE(list.contains("worker")) << list;
    EXPECT_FALSE(list.contains("server")) << list;
    panel->onDetach();
}

// #838: a collapsed parent shows how many children it has after its name, "(1)"; an expanded one does not.
TEST_F(ProcessesPanelRenderTest, ACollapsedParentShowsItsChildCountAndAnExpandedOneDoesNot)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("(1)")) << text;

    const ImGuiWindow* shell = ImGui::FindWindowByName("Shell");
    ASSERT_NE(shell, nullptr);
    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_UP "##CollapseAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    ASSERT_FALSE(text.contains("server")) << text;
    EXPECT_GT(text.find("(1)"), text.find("init")) << text; // init's one child, after its name
    EXPECT_NE(text.find("(1)"), std::string::npos) << text;

    ImGui::ActivateItemByID(ImHashStr(ICON_FA_ANGLES_DOWN "##ExpandAll", 0, shell->ID));
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    text = renderAndCapture(*panel);
    EXPECT_TRUE(text.contains("worker")) << text;
    EXPECT_FALSE(text.contains("(1)")) << text;
    panel->onDetach();
}

/// The x positions of the tree's guide lines (the theme stub's treeLines, used by nothing else) in the
/// last frame: one per line, the middle of each run of opaque vertices less than a few pixels apart.
[[nodiscard]] std::vector<float> guideLineXs()
{
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(UI::Theme::get().scheme().treeLines);
    std::vector<float> xs;
    if (const ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr)
    {
        for (const ImDrawList* list : drawData->CmdLists)
        {
            for (const ImDrawVert& vert : list->VtxBuffer)
            {
                if (vert.col == lineColor)
                {
                    xs.push_back(vert.pos.x);
                }
            }
        }
    }
    std::ranges::sort(xs);
    std::vector<float> lines;
    float runStart = 0.0F;
    for (std::size_t i = 0; i < xs.size(); ++i)
    {
        if (i == 0 || xs[i] - xs[i - 1] > 3.0F)
        {
            runStart = xs[i];
            lines.push_back(xs[i]);
        }
        lines.back() = (runStart + xs[i]) * 0.5F;
    }
    return lines;
}

// #838: each tree row draws a guide line for every level above it, through its ancestors' carets: worker
// (depth 2) shares init's line with server and adds server's. The roots and the list draw none.
TEST_F(ProcessesPanelRenderTest, TheTreeDrawsAGuideLineForEachLevelAboveARow)
{
    SelectionSink events;
    const auto panel = attachedPanel(events.sink());
    renderUntilLoaded(*panel);
    if (!panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    const std::vector<float> lines = guideLineXs();
    ASSERT_EQ(lines.size(), 2U);
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    const float step = ProcessTreeIndent::INDENT_PER_LEVEL_EM * ImGui::GetFontSize();
    EXPECT_NEAR(lines[1] - lines[0], step, 1.5F); // one level apart, at the font's indent step
    EXPECT_GT(lines[0], name.MinX);
    EXPECT_LT(lines[0], name.MinX + ImGui::GetStyle().CellPadding.x + ImGui::GetFrameHeight()); // in init's caret slot

    panel->toggleTreeView(); // the list: no tree, no lines
    static_cast<void>(renderAndCapture(*panel));
    static_cast<void>(renderAndCapture(*panel));
    EXPECT_TRUE(guideLineXs().empty());
    panel->onDetach();
}

// #838: a name too long for its row at depth 12 is cut short with an ellipsis, with the whole name as
// its tooltip, after every level's indent has shrunk alike to fit the deepest row.
TEST_F(ProcessesPanelRenderTest, ADeepLongNameIsCutShortWithItsWholeNameAsTheTooltip)
{
    constexpr std::uint64_t RSS = std::uint64_t{1} << 20U;
    constexpr int DEPTH = 12;
    const std::string longName = "a-process-name-far-too-long-for-the-name-column-at-this-depth.exe";
    std::vector<Platform::ProcessCounters> chain;
    for (int level = 0; level <= DEPTH; ++level)
    {
        const std::string processName = level == DEPTH ? longName : std::format("level-{}", level);
        chain.push_back(
            TestMocks::makeProcessCounters(level + 1, processName, 'S', 10, 5, 100U + static_cast<std::uint64_t>(level), RSS, level));
    }
    const auto panel = loadedTreePanel(std::move(chain));
    ASSERT_EQ(panel->processCount(), static_cast<std::size_t>(DEPTH + 1));
    const std::string text = renderAndCapture(*panel);
    ASSERT_TRUE(text.contains(longName)) << text; // logged whole, as a clipped cell logs its text
    ASSERT_TRUE(text.contains("level-0")) << text;

    // Down the right edge of the Name column, holding still long enough for a tooltip: only the long
    // name is clipped, so the only tooltip is its own, which logs the name once more.
    const ImGuiTable* table = processTable();
    ASSERT_NE(table, nullptr);
    const ImGuiTableColumn& name = table->Columns[static_cast<int>(toIndex(ProcessColumn::Name))];
    ImGuiIO& io = ImGui::GetIO();
    const auto occurrences = [&longName](const std::string& log)
    {
        std::size_t count = 0;
        for (std::size_t at = log.find(longName); at != std::string::npos; at = log.find(longName, at + 1))
        {
            ++count;
        }
        return count;
    };
    io.AddMousePosEvent(-100.0F, -100.0F); // hovering nothing: only the cell logs the name
    static_cast<void>(renderAndCapture(*panel));
    const std::size_t resting = occurrences(renderAndCapture(*panel));
    ASSERT_GE(resting, 1U);
    bool shown = false;
    for (int y = 20; y <= 400 && !shown; y += 4)
    {
        io.AddMousePosEvent(name.MaxX - 12.0F, static_cast<float>(y));
        for (int frame = 0; frame < 30 && !shown; ++frame) // past ImGuiHoveredFlags_DelayShort at 60 fps
        {
            shown = occurrences(renderAndCapture(*panel)) == resting + 1;
        }
    }
    EXPECT_TRUE(shown);
    panel->onDetach();
}

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
    auto gate = std::make_shared<std::latch>(1); // holds the factory, so no publication can arrive yet
    auto panel = std::make_unique<ProcessesPanel>(
        [gate]
        {
            gate->wait();
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1));
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
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
                    .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
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

// #3068: grouped by container, each container is a header row with its process count and Σ totals, its
// processes under it, the rest under Ungrouped last; clicking a header collapses it; Tree sets the group
// back to None.
TEST_F(ProcessesPanelRenderTest, GroupByContainerShowsHeadersWithTotalsAndCollapses)
{
    UserConfig::get().settings().processGroupBy = ProcessGrouping::GroupBy::Container;
    auto panel = std::make_unique<ProcessesPanel>(
        []
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            auto web = TestMocks::makeProcessCounters(100, "nginx", 'S', 200, 50, 1000, std::uint64_t{8} << 20U, 1);
            web.container = "docker:1a2b3c4d5e6f";
            auto worker = TestMocks::makeProcessCounters(101, "nginx-worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100);
            worker.container = "docker:1a2b3c4d5e6f";
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(web)
                .withProcess(worker);
            return ProcessesPanelPlatform{
                .probe = std::move(probe), .actions = std::make_unique<TestMocks::MockProcessActions>(), .icons = nullptr};
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

} // namespace
} // namespace App
