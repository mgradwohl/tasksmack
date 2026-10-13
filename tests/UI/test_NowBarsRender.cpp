/// @file test_NowBarsRender.cpp
/// @brief UI::Widgets::renderHistoryWithNowBars()'s other paths (#1395), headless: bars only (history
/// unavailable: no chart, one bar per value, each with its tooltip on hover), no bars (just the chart),
/// RenderMetrics capture (the Ctrl+Shift+M overlay's per-chart samples), a value strip in a row too
/// narrow for it, which falls back to compact labels, the table layout with compact spacing (its bars'
/// tooltips), a history chart drawn with anti-aliasing off, and where the value strip puts each label:
/// on the side of its series' Y axis, for a single-axis and a dual-axis chart (#1882).

#include "UI/ChartWidgets.h"
#include "UI/RenderMetrics.h"
#include "UI/SeriesLabelAlignment.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h> // GImGui->LogBuffer: the text a frame drew
#include <implot.h>

#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace UI::Widgets
{
namespace
{

constexpr float DISPLAY_WIDTH = 800.0F;
constexpr float DISPLAY_HEIGHT = 600.0F;
constexpr float PLOT_HEIGHT = 120.0F;

/// Where a frame of NowBarsRenderTest::stripFrame() put the chart's heading, its plot and its row.
struct StripGeometry
{
    float rowLeft = 0.0F;
    float headingRight = 0.0F;
    float headingTop = 0.0F;
    float headingBottom = 0.0F;
    float chartRight = 0.0F; ///< The chart's right edge: the row less the NowBar column
    float plotTop = 0.0F;
    float measuredEntryWidth = 0.0F; ///< Detail::valueStripEntryWidth() of the measured entry
};

/// The entry stripFrame() measures the width of: its head, tail and whether it is on the right axis.
struct MeasuredEntry
{
    std::string_view head;
    std::string_view tail;
    bool rightAxis = false;
};

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
    static Drawn
    frame(std::span<const NowBar> bars, bool barsOnly, NowBarValues values, float width = DISPLAY_WIDTH, bool compactSpacing = false)
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
            compactSpacing,
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

    /// One frame: a heading, then renderHistoryWithNowBars() under @p tableId with its value strip
    /// for @p bars, on a chart whose axes are @p axes. Leaves the frame's draw data for swatchAt().
    static StripGeometry
    stripFrame(const char* tableId, std::span<const NowBar> bars, SeriesLabelAlignment::ChartAxes axes = {}, MeasuredEntry measured = {})
    {
        StripGeometry geometry;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
        ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
        ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        geometry.rowLeft = ImGui::GetCursorScreenPos().x;
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        geometry.chartRight = geometry.rowLeft + rowWidth - nowBarsReservedWidth(bars.size(), 0, false, rowWidth);
        geometry.measuredEntryWidth = Detail::valueStripEntryWidth(measured.head, measured.tail, measured.rightAxis);
        ImGui::TextUnformatted("Heading");
        geometry.headingRight = ImGui::GetItemRectMax().x;
        geometry.headingTop = ImGui::GetItemRectMin().y;
        geometry.headingBottom = ImGui::GetItemRectMax().y;
        renderHistoryWithNowBars(
            tableId,
            PLOT_HEIGHT,
            [&geometry]
            {
                geometry.plotTop = ImGui::GetCursorScreenPos().y;
                if (ImPlot::BeginPlot("##StripPlot", ImVec2(-1.0F, PLOT_HEIGHT)))
                {
                    ImPlot::EndPlot();
                }
            },
            bars,
            false,
            0,
            false,
            NowBarValues::Strip,
            {},
            axes);
        ImGui::End();
        ImGui::Render();
        return geometry;
    }

    /// The top-left corner of the value-strip swatch drawn in @p color above @p belowY (the plot's top,
    /// so the NowBar in the same colour is left out), from the last frame's draw data; FLT_MAX in both
    /// coordinates if none was drawn.
    [[nodiscard]] static ImVec2 swatchAt(const ImVec4& color, float belowY)
    {
        const ImU32 packed = ImGui::ColorConvertFloat4ToU32(color);
        ImVec2 corner(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        const ImVector<ImDrawList*>& lists = ImGui::GetDrawData()->CmdLists;
        int listIndex = 0;
        while (listIndex < lists.Size)
        {
            const ImVector<ImDrawVert>& vertices = lists[listIndex]->VtxBuffer;
            int vertexIndex = 0;
            while (vertexIndex < vertices.Size)
            {
                const ImDrawVert& vertex = vertices[vertexIndex];
                if (vertex.col == packed && vertex.pos.y < belowY && vertex.pos.x < corner.x)
                {
                    corner = vertex.pos;
                }
                ++vertexIndex;
            }
            ++listIndex;
        }
        return corner;
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

TEST_F(NowBarsRenderTest, ABarBesideTheChartSaysWhatItIsOnHoverInACompactTable)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(frame(bars, false, NowBarValues::None, DISPLAY_WIDTH, /*compactSpacing=*/true));
    // The bars sit in the table's right-hand column: sweep its lower half until a bar's tooltip shows.
    bool shown = false;
    for (int column = 0; column < 40 && !shown; ++column)
    {
        for (int row = 1; row <= 6 && !shown; ++row)
        {
            ImGui::GetIO().AddMousePosEvent(DISPLAY_WIDTH - 4.0F - (4.0F * static_cast<float>(column)), 20.0F * static_cast<float>(row));
            static_cast<void>(frame(bars, false, NowBarValues::None, DISPLAY_WIDTH, true));
            const std::string text = frame(bars, false, NowBarValues::None, DISPLAY_WIDTH, true).text;
            shown = text.contains("CPU Total: 42%") || text.contains("CPU User: 30%");
        }
    }
    EXPECT_TRUE(shown);
}

// #1882: on a chart with one Y axis, its scale on the left, the value strip's labels are left-aligned:
// the first follows the chart's heading on its line, the next after it, all in the chart's left half.
TEST_F(NowBarsRenderTest, ASingleAxisChartsLabelsAreLeftAligned)
{
    const std::array<NowBar, 2> bars = twoBars();
    static_cast<void>(stripFrame("SingleAxisStrip", bars));
    const StripGeometry geometry = stripFrame("SingleAxisStrip", bars);
    const auto total = swatchAt(bars[0].color, geometry.plotTop);
    const auto user = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(total.x, DISPLAY_WIDTH); // drawn
    ASSERT_LT(user.x, DISPLAY_WIDTH);  // drawn

    const float entryGap = ImGui::GetStyle().ItemSpacing.x * 2.0F;
    EXPECT_NEAR(total.x, geometry.headingRight + entryGap, 1.0F);
    EXPECT_GE(total.y, geometry.headingTop); // on the heading's line
    EXPECT_LT(total.y, geometry.headingBottom);
    EXPECT_GT(user.x, total.x);
    EXPECT_NEAR(user.y, total.y, 0.5F);
    EXPECT_LT(user.x, (geometry.rowLeft + geometry.chartRight) * 0.5F);
}

// #1882: on a dual-axis chart the left axis's series is left-aligned, after the heading, and the
// right axis's series (its label ends in SECONDARY_AXIS_MARK) right-aligned, its entry ending at the
// chart's right edge -- on the same line.
TEST_F(NowBarsRenderTest, ADualAxisChartsLabelsAlignToTheirOwnAxes)
{
    const std::array<NowBar, 2> bars{
        NowBar{.valueText = "512", .label = "Threads", .tooltipText = {}, .value01 = 0.5, .color = ImVec4(0.2F, 0.6F, 1.0F, 1.0F)},
        NowBar{.valueText = "12.0/s",
               .label = "Page Faults \xE2\x86\x92", // " →": SECONDARY_AXIS_MARK
               .tooltipText = {},
               .value01 = 0.1,
               .color = ImVec4(1.0F, 0.6F, 0.2F, 1.0F)}};
    const MeasuredEntry faults{.head = "Page Faults", .tail = "12.0/s", .rightAxis = true};
    static_cast<void>(stripFrame("DualAxisStrip", bars, {}, faults));
    const StripGeometry geometry = stripFrame("DualAxisStrip", bars, {}, faults);
    const auto threads = swatchAt(bars[0].color, geometry.plotTop);
    const auto faultsSwatch = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(threads.x, DISPLAY_WIDTH);      // drawn
    ASSERT_LT(faultsSwatch.x, DISPLAY_WIDTH); // drawn

    const float entryGap = ImGui::GetStyle().ItemSpacing.x * 2.0F;
    EXPECT_NEAR(threads.x, geometry.headingRight + entryGap, 1.0F);
    EXPECT_NEAR(faultsSwatch.x, geometry.chartRight - geometry.measuredEntryWidth, 1.0F);
    EXPECT_GE(faultsSwatch.y, geometry.headingTop); // both on the heading's line
    EXPECT_LT(faultsSwatch.y, geometry.headingBottom);
    EXPECT_NEAR(faultsSwatch.y, threads.y, 0.5F);
}

// #1882: a chart whose only scale is on the right right-aligns every label, the last ending at the
// chart's right edge.
TEST_F(NowBarsRenderTest, ARightAxisChartsLabelsAreRightAligned)
{
    const std::array<NowBar, 2> bars = twoBars();
    const SeriesLabelAlignment::ChartAxes rightAxis{.primary = SeriesLabelAlignment::AxisSide::Right};
    const MeasuredEntry user{.head = "CPU User", .tail = "30%", .rightAxis = false};
    static_cast<void>(stripFrame("RightAxisStrip", bars, rightAxis, user));
    const StripGeometry geometry = stripFrame("RightAxisStrip", bars, rightAxis, user);
    const auto total = swatchAt(bars[0].color, geometry.plotTop);
    const auto userSwatch = swatchAt(bars[1].color, geometry.plotTop);
    ASSERT_LT(total.x, DISPLAY_WIDTH);      // drawn
    ASSERT_LT(userSwatch.x, DISPLAY_WIDTH); // drawn

    EXPECT_NEAR(userSwatch.x, geometry.chartRight - geometry.measuredEntryWidth, 1.0F);
    EXPECT_LT(total.x, userSwatch.x);
    EXPECT_GT(total.x, (geometry.rowLeft + geometry.chartRight) * 0.5F);
}

TEST_F(NowBarsRenderTest, AChartWithAntiAliasingOffClearsItsLinesFlagsOnlyWhileItDraws)
{
    setChartAntiAliasingEnabled(false);
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0F, 0.0F));
    ImGui::SetNextWindowSize(ImVec2(DISPLAY_WIDTH, DISPLAY_HEIGHT));
    ImGui::Begin("Charts", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const ImDrawList* list = ImGui::GetWindowDrawList();
    const ImDrawListFlags before = list->Flags;
    ImDrawListFlags during = 0;
    bool active = false;
    {
        const HistoryChart chart(percentHistoryConfig("##AntiAliasingOff", -60.0, 0.0));
        active = chart.active();
        during = list->Flags;
    }
    const ImDrawListFlags after = list->Flags;
    ImGui::End();
    ImGui::Render();
    setChartAntiAliasingEnabled(true);

    ASSERT_TRUE(active);
    EXPECT_EQ(during & CHART_ANTI_ALIASING_FLAGS_MASK, 0); // the chart's lines drawn without AA
    EXPECT_EQ(after, before);                              // the window's own flags given back
}

} // namespace
} // namespace UI::Widgets
