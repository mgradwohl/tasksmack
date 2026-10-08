/// @file test_RenderMetricsOverlay.cpp
/// @brief RenderMetrics::renderOverlay() run headless (#1395): opening and closing the overlay turns
/// capture on and off (closing drops every captured frame), the window lists the last frame's charts,
/// and an overlay left beyond the edge of a main window that shrank is pulled back inside it.

#include "UI/RenderMetrics.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <string>

namespace UI
{
namespace
{

constexpr float DISPLAY_WIDTH = 1600.0F;
constexpr float DISPLAY_HEIGHT = 1200.0F;

class RenderMetricsOverlayTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_ImGui = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
        io.Fonts->AddFontDefault();
        RenderMetrics::get().setEnabled(false);
    }

    void TearDown() override
    {
        RenderMetrics::get().setEnabled(false);
        RenderMetrics::get().setScenario("");
        ImGui::DestroyContext(m_ImGui);
    }

    static void frame(bool* open)
    {
        ImGui::NewFrame();
        RenderMetrics::get().renderOverlay(open);
        ImGui::Render();
    }

    /// One frame of the overlay, returning all the text it drew, captured through ImGui's own logging
    /// (LogToBuffer(), which also turns clipping off) as the headless view tests do.
    [[nodiscard]] static std::string frameAndCapture(bool* open)
    {
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        RenderMetrics::get().renderOverlay(open);
        std::string captured = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        return captured;
    }

    [[nodiscard]] static ImGuiWindow* overlayWindow()
    {
        return ImGui::FindWindowByName("Render Metrics");
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
};

TEST_F(RenderMetricsOverlayTest, NoOpenFlagDisablesCaptureAndDrawsNoWindow)
{
    RenderMetrics::get().setEnabled(true);
    frame(nullptr);
    EXPECT_FALSE(RenderMetrics::get().enabled());
    EXPECT_EQ(overlayWindow(), nullptr);
}

TEST_F(RenderMetricsOverlayTest, OpeningEnablesCaptureAndClosingDropsTheFrames)
{
    auto& metrics = RenderMetrics::get();
    bool open = true;
    frame(&open);
    ASSERT_TRUE(metrics.enabled());
    ASSERT_NE(overlayWindow(), nullptr);
    EXPECT_TRUE(overlayWindow()->Active);

    metrics.beginFrame(1);
    metrics.record("##Cpu", 120, 3.5, 1);
    metrics.record("##Memory", 80, 1.25, 1);
    metrics.beginFrame(2);
    ASSERT_EQ(metrics.lastFrame().size(), 2U);
    frame(&open);
    const std::string text = frameAndCapture(&open);

    // The summary totals the last frame's charts, and the table lists each one.
    EXPECT_NE(text.find("Charts: 200 vertices"), std::string::npos) << text;
    EXPECT_NE(text.find("(2 charts)"), std::string::npos) << text;
    const auto cpu = text.find("##Cpu");
    const auto memory = text.find("##Memory");
    ASSERT_NE(cpu, std::string::npos) << text;
    ASSERT_NE(memory, std::string::npos) << text;
    // Sorted by the default sort column, Vertices, ascending: Memory (80) before Cpu (120).
    EXPECT_LT(memory, cpu) << text;

    open = false;
    frame(&open);
    EXPECT_FALSE(metrics.enabled());
    EXPECT_TRUE(metrics.lastFrame().empty());
}

TEST_F(RenderMetricsOverlayTest, FitsAndStaysInsideAMainWindowThatShrinks)
{
    bool open = true;
    frame(&open);
    frame(&open);
    ImGuiWindow* window = overlayWindow();
    ASSERT_NE(window, nullptr);
    // Dragged to the bottom-right corner of the large window.
    ImGui::SetWindowPos(window, ImVec2(DISPLAY_WIDTH - window->Size.x, DISPLAY_HEIGHT - window->Size.y));
    frame(&open);

    constexpr float SMALL_WIDTH = 700.0F;
    constexpr float SMALL_HEIGHT = 500.0F;
    ImGui::GetIO().DisplaySize = ImVec2(SMALL_WIDTH, SMALL_HEIGHT);
    for (int i = 0; i < 3; ++i)
    {
        frame(&open);
    }

    window = overlayWindow();
    ASSERT_NE(window, nullptr);
    EXPECT_LE(window->Size.x, SMALL_WIDTH);
    EXPECT_LE(window->Size.y, SMALL_HEIGHT);
    EXPECT_GE(window->Pos.x, 0.0F);
    EXPECT_GE(window->Pos.y, 0.0F);
    EXPECT_LE(window->Pos.x + window->Size.x, SMALL_WIDTH + 0.5F);
    EXPECT_LE(window->Pos.y + window->Size.y, SMALL_HEIGHT + 0.5F);
}

} // namespace
} // namespace UI
