/// @file test_CpuDetailsBlockRender.cpp
/// @brief The Overview's CPU Details block rendered headless (#809): collapsed it is exactly one
/// text line, the old header's height; expanded it is taller; and the fill layout the Overview's
/// charts share their height through counts whichever height it actually took.

#include "App/Panels/CpuDetailsBlock.h"
#include "App/Panels/CpuDetailsText.h"
#include "Domain/SystemSnapshot.h"
#include "UI/FillPlotLayout.h"

#include <gtest/gtest.h>
#include <imgui.h>

#include <functional>
#include <string>
#include <vector>

namespace App
{
namespace
{

constexpr float WINDOW_WIDTH = 1900.0F;
constexpr float WINDOW_HEIGHT = 1000.0F;

class CpuDetailsBlockRenderTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Context = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT);
        io.DeltaTime = 1.0F / 60.0F;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // No renderer: ImGui owns the atlas
        io.Fonts->AddFontDefault();

        m_Snapshot.cpuModel = "Test CPU";
        m_Snapshot.coreCount = 16;
        m_Snapshot.cpuFreqMHz = 3710;
        m_Snapshot.uptimeSeconds = 3600;
        m_Snapshot.memoryTotalBytes = 32ULL * 1024 * 1024 * 1024;
        m_Snapshot.cpuDetails.sockets = 1;
        m_Snapshot.cpuDetails.physicalCores = 16;
        m_Snapshot.cpuDetails.performanceCores = 6;
        m_Snapshot.cpuDetails.efficiencyCores = 10;
        m_Snapshot.cpuDetails.logicalProcessors = 16;
        m_Snapshot.cpuDetails.baseSpeedMHz = 2000;
        CpuDetailsText::Inputs inputs;
        inputs.snapshot = &m_Snapshot;
        inputs.hasCpuFreq = true;
        inputs.hasUptime = true;
        m_Rows = CpuDetailsText::buildRows(inputs);
        m_Summary = CpuDetailsText::collapsedSummary(inputs);
    }

    void TearDown() override
    {
        ImGui::DestroyContext(m_Context);
    }

    /// One frame of a full-size window running @p body.
    static void runFrame(const std::function<void()>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));
        ImGui::Begin("Overview", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        body();
        ImGui::End();
        ImGui::Render();
    }

    /// The height the block took in a frame, rendered @p expanded.
    float blockHeight(bool expanded)
    {
        float height = 0.0F;
        runFrame(
            [&]
            {
                const float top = ImGui::GetCursorPosY();
                (void) CpuDetailsBlock::render(content(expanded), m_Measured);
                height = ImGui::GetCursorPosY() - top;
            });
        return height;
    }

    [[nodiscard]] CpuDetailsBlock::Content content(bool expanded) const
    {
        return {.cpuModel = m_Snapshot.cpuModel, .collapsedSummary = m_Summary, .rows = m_Rows, .rowsGeneration = 1, .expanded = expanded};
    }

    ImGuiContext* m_Context = nullptr;
    Domain::SystemSnapshot m_Snapshot;
    std::vector<CpuDetailsText::Row> m_Rows;
    std::string m_Summary;
    CpuDetailsBlock::MeasuredRows m_Measured;
};

TEST_F(CpuDetailsBlockRenderTest, CollapsedIsOneTextLine)
{
    float lineHeight = 0.0F;
    runFrame([&] { lineHeight = ImGui::GetTextLineHeightWithSpacing(); });
    // As tall as the one-line header the block replaced: a line of text and the spacing after it
    EXPECT_FLOAT_EQ(blockHeight(false), lineHeight);
}

TEST_F(CpuDetailsBlockRenderTest, ExpandedShowsTheRowsBelowTheHeading)
{
    float lineHeight = 0.0F;
    runFrame([&] { lineHeight = ImGui::GetTextLineHeight(); });
    const float collapsed = blockHeight(false);
    const float expanded = blockHeight(true);
    EXPECT_GT(expanded, collapsed + lineHeight); // At least one row of facts
}

TEST_F(CpuDetailsBlockRenderTest, TheChartsHeightBudgetCountsTheBlocksActualHeight)
{
    // The Overview renders the block inside its FillPlotLayout scope; what the scope measures as
    // non-plot height is what the charts' shared height is reduced by
    const auto measuredNonPlot = [&](bool expanded)
    {
        UI::Widgets::PlotFillState state;
        float height = 0.0F;
        runFrame(
            [&]
            {
                const UI::Widgets::FillPlotLayout fill(state);
                const float top = ImGui::GetCursorPosY();
                (void) CpuDetailsBlock::render(content(expanded), m_Measured);
                height = ImGui::GetCursorPosY() - top;
            });
        EXPECT_FLOAT_EQ(state.nonPlotHeight, height);
        return state.nonPlotHeight;
    };
    const float collapsed = measuredNonPlot(false);
    const float expanded = measuredNonPlot(true);
    EXPECT_LT(collapsed, expanded); // Collapsing gives the charts the difference
}

TEST_F(CpuDetailsBlockRenderTest, ClickingTheHeadingIsReportedOnlyOnClick)
{
    bool toggled = true;
    runFrame([&] { toggled = CpuDetailsBlock::render(content(true), m_Measured); });
    EXPECT_FALSE(toggled); // No input this frame
}

} // namespace
} // namespace App
