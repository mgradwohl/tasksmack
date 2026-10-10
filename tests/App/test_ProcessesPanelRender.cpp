/// @file test_ProcessesPanelRender.cpp
/// @brief The Processes panel as a whole (#1546), run headless with a mock process probe and mock
/// actions through its probe-factory seam: the table lists the probe's processes, the tree view nests
/// a child under its parent (and toggling it before the table's first frame no longer asserts, #1656),
/// and an inactive panel draws nothing. Selection raises application events, which go to the test's
/// event sink through the panel's second seam: keyboard selection announces the selected process.

#include "App/Panels/ProcessesPanel.h"
#include "Core/ApplicationEvents.h"
#include "Core/Event.h"
#include "Mocks/MockProbes.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
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
    }

    void TearDown() override
    {
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// An attached panel reading three processes: init (1), server (100) and its child worker (101). The
    /// events it raises go to @p raiseEvent, when given.
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
            std::move(raiseEvent));
        panel->onAttach();
        Core::ActiveTabChangedEvent shown("Processes");
        panel->onEvent(shown);
        return panel;
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

} // namespace
} // namespace App
