#pragma once

// The fixture and helpers test_ProcessesPanelRender*.cpp share (#3850: the one 5,600-line file split by feature).
// In namespace App, not an anonymous one: gtest requires every test of a suite to use the same fixture class, and an
// anonymous namespace would give each test file its own. The functions are inline, defined once for all of them.

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

namespace App::ProcessesPanelRenderTestDetail
{

/// Look up on VirusTotal's client in these tests (#3094): the shared fake (#3849).
using FakeLookupClient = TestMocks::FakeHttpsClient;

} // namespace App::ProcessesPanelRenderTestDetail

namespace App
{

constexpr float PANEL_RENDER_DISPLAY_WIDTH = 1600.0F;
constexpr float PANEL_RENDER_DISPLAY_HEIGHT = 1200.0F;

/// onAttach(), then the seed the sampler thread takes (#2731), adopted as the first frame after it
/// would: the tests below read the rows, actions and capabilities as they stand after it.
inline void attachAndSeed(ProcessesPanel& panel)
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
        io.DisplaySize = ImVec2(PANEL_RENDER_DISPLAY_WIDTH, PANEL_RENDER_DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
        m_Settings = UserConfig::get().settings(); // the menus write the column and meter settings
        // A panel without a UDP collector publishes "unavailable" process-wide (#3366): each test starts from none, and
        // TearDown() puts back what was there, so no other suite's test depends on these having run.
        m_UdpStatus = App::udpCountingStatus();
        App::publishUdpCountingStatus({});
    }

    void TearDown() override
    {
        App::publishUdpCountingStatus(m_UdpStatus);
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
                    .probe = std::move(probe),
                    .actions = std::make_unique<TestMocks::MockProcessActions>(),
                    .icons = nullptr,
                };
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

    /// An attached panel reading three processes with Windows owners (#3328): init (1) as PC\bob, server (100)
    /// as PC\alice and worker (101) as CORP\alice, so two accounts share the name alice.
    [[nodiscard]] static std::unique_ptr<ProcessesPanel> attachedPanelWithOwners()
    {
        auto panel = std::make_unique<ProcessesPanel>(
            []
            {
                const auto owned = [](Platform::ProcessCounters counters, std::string domain, std::string user)
                {
                    counters.userDomain = std::move(domain);
                    counters.user = std::move(user);
                    return counters;
                };
                auto probe = std::make_unique<TestMocks::MockProcessProbe>();
                probe
                    ->withProcess(
                        owned(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0), "PC", "bob"))
                    .withProcess(
                        owned(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1), "PC", "alice"))
                    .withProcess(owned(
                        TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100), "CORP", "alice"));
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
        io.AddMousePosEvent(PANEL_RENDER_DISPLAY_WIDTH * 0.5F, PANEL_RENDER_DISPLAY_HEIGHT * 0.5F); // hovered: navigation is armed
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
        ImGui::SetNextWindowSize(ImVec2(PANEL_RENDER_DISPLAY_WIDTH, PANEL_RENDER_DISPLAY_HEIGHT));
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
        io.AddMousePosEvent(PANEL_RENDER_DISPLAY_WIDTH * 0.5F, PANEL_RENDER_DISPLAY_HEIGHT * 0.5F); // hovered: navigation is armed
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
                    .probe = std::move(probe),
                    .actions = std::make_unique<TestMocks::MockProcessActions>(),
                    .icons = nullptr,
                };
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
    App::UdpCountingStatus m_UdpStatus;
    TestMocks::FakeClipboard m_Clipboard;
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

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

/// An attached panel reading init (1), server (100) and worker (101), each with Status @p status, over mock
/// actions that can turn Efficiency mode on and off (#3071); @p actions is left pointing at them.
[[nodiscard]] inline std::unique_ptr<ProcessesPanel> efficiencyModePanel(const std::string& status, TestMocks::MockProcessActions*& actions)
{
    auto panel = std::make_unique<ProcessesPanel>(
        // NOLINTNEXTLINE(bugprone-exception-escape): MSVC's debug std::string copy allocates its container proxy; a throw there is a test failure
        [&actions, status]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            for (auto counters : {
                     TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0),
                     TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1),
                     TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100),
                 })
            {
                counters.status = status;
                probe->withProcess(counters);
            }
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities({.canTerminate = true, .canKill = true, .canSetEfficiencyMode = true});
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    return panel;
}

/// As attachedPanelWithActions(), with @p caps for the mock actions and the tree view on when @p tree.
[[nodiscard]] inline std::unique_ptr<ProcessesPanel>
panelWithCapabilities(const Platform::ProcessActionCapabilities& caps, bool tree, TestMocks::MockProcessActions*& actions)
{
    auto panel = std::make_unique<ProcessesPanel>(
        [&actions, caps]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities(caps);
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event&) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    if (tree && !panel->treeViewEnabled())
    {
        panel->toggleTreeView();
    }
    return panel;
}

/// The start time attachedPanelWithActions()' mock gives @p pid's process.
[[nodiscard]] constexpr std::uint64_t mockStartTicks(std::int32_t pid) noexcept
{
    if (pid == 1)
    {
        return 100;
    }
    return pid == 100 ? 1000 : 1100;
}

/// A panel reading init (1), server (100) and worker (101), with mock actions that have @p caps from the
/// start (the panel takes the capabilities in when it adopts the platform); @p actions points at them.
[[nodiscard]] inline std::unique_ptr<ProcessesPanel> panelWithCapabilities(Platform::ProcessActionCapabilities caps,
                                                                           TestMocks::MockProcessActions*& actions)
{
    auto panel = std::make_unique<ProcessesPanel>(
        [caps, &actions]
        {
            auto probe = std::make_unique<TestMocks::MockProcessProbe>();
            probe->withProcess(TestMocks::makeProcessCounters(1, "init", 'S', 10, 5, 100, std::uint64_t{1} << 20U, 0))
                .withProcess(TestMocks::makeProcessCounters(100, "server", 'R', 200, 50, 1000, std::uint64_t{8} << 20U, 1))
                .withProcess(TestMocks::makeProcessCounters(101, "worker", 'S', 30, 10, 1100, std::uint64_t{2} << 20U, 100));
            auto mock = std::make_unique<TestMocks::MockProcessActions>();
            mock->setCapabilities(caps);
            actions = mock.get();
            return ProcessesPanelPlatform{.probe = std::move(probe), .actions = std::move(mock), .icons = nullptr};
        },
        [](Core::Event& /*event*/) {});
    attachAndSeed(*panel);
    Core::ActiveTabChangedEvent shown("Processes");
    panel->onEvent(shown);
    return panel;
}

} // namespace App
