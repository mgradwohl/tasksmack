/// @file test_NowBarsRender.cpp
/// @brief UI::Widgets::renderHistoryWithNowBars()'s other paths (#1395), headless: bars only (history
/// unavailable: no chart, one bar per value, each with its tooltip on hover), no bars (just the chart),
/// RenderMetrics capture (the Ctrl+Shift+M overlay's per-chart samples), and a value strip in a row too
/// narrow for it, which falls back to compact labels.

#include "UI/ChartWidgets.h"
#include "UI/RenderMetrics.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer: the text a frame drew
#include <implot.h>

#include <array>
#include <span>
#include <string>

namespace UI::Widgets
{
namespace
{

constexpr float DISPLAY_WIDTH = 800.0F;
constexpr float DISPLAY_HEIGHT = 600.0F;
constexpr float PLOT_HEIGHT = 120.0F;

class NowBarsRenderTest : public ::testing::Test
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
        RenderMetrics::get().setEnabled(false);
        ImPlot::DestroyContext(m_ImPlot);
        ImGui::DestroyContext(m_ImGui);
    }

    /// Two bars, as the CPU chart's: Total and User. Plain tooltips: "label: value".
    [[nodiscard]] static std::array<NowBar, 2> twoBars()
    {
        return {
            NowBar{.valueText = "42%", .label = "CPU Total", .tooltipText = {}, .value01 = 0.42, .color = ImVec4(0.2F, 0.6F, 1.0F, 1.0F)},
            NowBar{.valueText = "30%", .label = "CPU User", .tooltipText = {}, .value01 = 0.30, .color = ImVec4(0.4F, 0.8F, 1.0F, 1.0F)}};
    }

    struct Drawn
    {
        bool plotCalled = false;
        std::string text;
        int vertices = 0;
    };

    /// One frame: a window @p width wide holding renderHistoryWithNowBars(), with the text it drew.
    static Drawn frame(std::span<const NowBar> bars, bool barsOnly, NowBarValues values, float width = DISPLAY_WIDTH)
    {
        Drawn drawn;
        ImGui::NewFrame();
        RenderMetrics::get().beginFrame(ImGui::GetFrameCount());
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(width, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::LogToBuffer();
        renderHistoryWithNowBars(
            "TestChart",
            PLOT_HEIGHT,
            [&drawn]
            {
                drawn.plotCalled = true;
                if (ImPlot::BeginPlot("##TestPlot", ImVec2(-1.0F, PLOT_HEIGHT)))
                {
                    ImPlot::EndPlot();
                }
            },
            bars,
            barsOnly,
            0,
            false,
            values);
        drawn.text = GImGui->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::Render();
        for (const ImDrawList* list : ImGui::GetDrawData()->CmdLists)
        {
            drawn.vertices += list->VtxBuffer.Size;
        }
        return drawn;
    }

  private:
    ImGuiContext* m_ImGui = nullptr;
    ImPlotContext* m_ImPlot = nullptr;
};

TEST_F(NowBarsRenderTest, WithoutBarsItIsJustTheChart)
{
    const Drawn drawn = frame({}, false, NowBarValues::Strip);
    EXPECT_TRUE(drawn.plotCalled);
}

TEST_F(NowBarsRenderTest, BarsOnlyDrawsTheBarsAndNoChart)
{
    const std::array<NowBar, 2> bars = twoBars();
    const Drawn drawn = frame(bars, true, NowBarValues::None);
    EXPECT_FALSE(drawn.plotCalled); // history unavailable: no chart at all
    EXPECT_GT(drawn.vertices, 0);   // but the bars are drawn
}

TEST_F(NowBarsRenderTest, ABarSaysWhatItIsOnHover)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, true, NowBarValues::None));
    // Down the first bar's column until its tooltip shows: "label: value" (no richer text was given).
    bool shown = false;
    for (int step = 1; step <= 40 && !shown; ++step) // every 4 px down past the bar's foot
    {
        ImGui::GetIO().AddMousePosEvent(14.0F, 4.0F * static_cast<float>(step));
        static_cast<void>(frame(bars, true, NowBarValues::None));
        shown = frame(bars, true, NowBarValues::None).text.contains("CPU Total: 42%");
    }
    EXPECT_TRUE(shown);
}

TEST_F(NowBarsRenderTest, RenderMetricsRecordsTheChartAndItsBarsWhenOn)
{
    RenderMetrics::get().setEnabled(true);
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, true, NowBarValues::None));
    static_cast<void>(frame(bars, true, NowBarValues::None)); // the first frame's samples are published by the next
    bool barsRecorded = false;
    for (const ChartRenderSample& sample : RenderMetrics::get().lastFrame())
    {
        barsRecorded = barsRecorded || sample.id.contains("TestChart/bars");
    }
    EXPECT_TRUE(barsRecorded);
}

TEST_F(NowBarsRenderTest, ANarrowRowShortensTheStripEntriesInsteadOfDroppingThem)
{
    const std::array<NowBar, 2> bars = twoBars();
    // Wide enough: every entry whole.
    static_cast<void>(frame(bars, false, NowBarValues::Strip));
    const Drawn wide = frame(bars, false, NowBarValues::Strip);
    EXPECT_TRUE(wide.text.contains("CPU Total : 42%")) << wide.text;
    EXPECT_TRUE(wide.text.contains("CPU User : 30%")) << wide.text;
    // Too narrow for "CPU Total : 42%": the entry keeps its name and fits the rest with an ellipsis.
    static_cast<void>(frame(bars, false, NowBarValues::Strip, 140.0F));
    const Drawn narrow = frame(bars, false, NowBarValues::Strip, 140.0F);
    EXPECT_TRUE(narrow.plotCalled);
    EXPECT_TRUE(narrow.text.contains("CPU Total")) << narrow.text;
    EXPECT_TRUE(narrow.text.contains("\xE2\x80\xA6")) << narrow.text; // U+2026
}

} // namespace
} // namespace UI::Widgets
